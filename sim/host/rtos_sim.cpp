// FreeRTOS on host threads. Ticks are milliseconds of the sim clock
// (sim_clock.h), so `speed` in the control channel speeds every task up alike.
#include "sim_rtos.h"
#include "sim_clock.h"

#include <condition_variable>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <pthread.h>

namespace {

std::recursive_mutex g_critical;

struct Task {
    std::string name;
    std::mutex m;
    std::condition_variable cv;
    uint32_t notify = 0;
};
thread_local Task* t_self = nullptr;
std::mutex g_tasksMutex;
std::vector<Task*> g_tasks;

struct Sem {
    enum Kind { MUTEX, RECURSIVE, BINARY, COUNTING } kind;
    std::mutex m;
    std::condition_variable cv;
    unsigned count = 0, max = 1;
    std::thread::id owner;
    unsigned depth = 0;
};

// Wait on `cv` until `ready()` or `ticks` of sim time pass.
template <typename Pred>
bool waitFor(std::unique_lock<std::mutex>& lk, std::condition_variable& cv, TickType_t ticks, Pred ready) {
    if (ticks == portMAX_DELAY) { cv.wait(lk, ready); return true; }
    if (ticks == 0) return ready();
    return cv.wait_for(lk, sim_clock::realDuration(ticks), ready);
}

struct Queue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::vector<uint8_t>> items;
    size_t len, size;
};

struct Ring {
    std::mutex m;
    std::condition_variable cv;
    size_t cap, used = 0;
    std::deque<std::vector<uint8_t>*> ready;
    std::map<void*, std::vector<uint8_t>*> pending;   // acquired, not yet complete
};

}  // namespace

extern "C" {

void sim_critical_enter(void) { g_critical.lock(); }
void sim_critical_exit(void) { g_critical.unlock(); }

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t, void* arg,
                                   UBaseType_t, TaskHandle_t* out, BaseType_t) {
    Task* t = new Task();
    t->name = name ? name : "task";
    {
        std::lock_guard<std::mutex> lk(g_tasksMutex);
        g_tasks.push_back(t);
    }
    if (out) *out = t;
    std::thread([fn, arg, t] {
        t_self = t;
        pthread_setname_np(t->name.c_str());
        fn(arg);
    }).detach();
    return pdPASS;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                       UBaseType_t prio, TaskHandle_t* out) {
    return xTaskCreatePinnedToCore(fn, name, stack, arg, prio, out, 0);
}

void vTaskDelay(TickType_t ticks) { sim_clock::sleepMs(ticks ? ticks : 1); }

void vTaskDelete(TaskHandle_t t) {
    // Only ever called with nullptr (a task ending itself) in src/: park the
    // thread forever rather than unwinding through firmware frames.
    if (!t || t == t_self) for (;;) std::this_thread::sleep_for(std::chrono::hours(24));
}

TickType_t xTaskGetTickCount(void) { return (TickType_t)sim_clock::millis(); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return t_self; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t) { return 4096; }

BaseType_t xTaskNotifyGive(TaskHandle_t h) {
    Task* t = (Task*)h;
    if (!t) return pdFAIL;
    std::lock_guard<std::mutex> lk(t->m);
    ++t->notify;
    t->cv.notify_all();
    return pdPASS;
}

void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t*) { xTaskNotifyGive(t); }

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait) {
    Task* t = t_self;
    if (!t) { vTaskDelay(wait == portMAX_DELAY ? 1000 : wait); return 0; }
    std::unique_lock<std::mutex> lk(t->m);
    waitFor(lk, t->cv, wait, [t] { return t->notify > 0; });
    uint32_t v = t->notify;
    if (v) t->notify = clear ? 0 : v - 1;
    return v;
}

static SemaphoreHandle_t makeSem(Sem::Kind k, unsigned max, unsigned initial) {
    Sem* s = new Sem();
    s->kind = k;
    s->max = max;
    s->count = initial;
    return s;
}

SemaphoreHandle_t xSemaphoreCreateMutex(void) { return makeSem(Sem::MUTEX, 1, 1); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void) { return makeSem(Sem::RECURSIVE, 1, 1); }
SemaphoreHandle_t xSemaphoreCreateBinary(void) { return makeSem(Sem::BINARY, 1, 0); }
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial) {
    return makeSem(Sem::COUNTING, max, initial);
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t h, TickType_t wait) {
    Sem* s = (Sem*)h;
    if (!s) return pdFAIL;
    std::unique_lock<std::mutex> lk(s->m);
    if (s->kind == Sem::RECURSIVE && s->depth && s->owner == std::this_thread::get_id()) {
        ++s->depth;
        return pdTRUE;
    }
    if (!waitFor(lk, s->cv, wait, [s] { return s->count > 0; })) return pdFALSE;
    --s->count;
    s->owner = std::this_thread::get_id();
    s->depth = 1;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t h) {
    Sem* s = (Sem*)h;
    if (!s) return pdFAIL;
    std::lock_guard<std::mutex> lk(s->m);
    if (s->kind == Sem::RECURSIVE && s->depth > 1) { --s->depth; return pdTRUE; }
    if (s->count >= s->max) return pdFALSE;
    ++s->count;
    s->depth = 0;
    s->owner = std::thread::id();
    s->cv.notify_one();
    return pdTRUE;
}

BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t w) { return xSemaphoreTake(s, w); }
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s) { return xSemaphoreGive(s); }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t*) { return xSemaphoreGive(s); }
void vSemaphoreDelete(SemaphoreHandle_t s) { delete (Sem*)s; }

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t size) {
    Queue* q = new Queue();
    q->len = len;
    q->size = size;
    return q;
}

BaseType_t xQueueSend(QueueHandle_t h, const void* item, TickType_t wait) {
    Queue* q = (Queue*)h;
    std::unique_lock<std::mutex> lk(q->m);
    if (!waitFor(lk, q->cv, wait, [q] { return q->items.size() < q->len; })) return pdFAIL;
    q->items.emplace_back((const uint8_t*)item, (const uint8_t*)item + q->size);
    q->cv.notify_all();
    return pdPASS;
}
BaseType_t xQueueSendToBack(QueueHandle_t q, const void* item, TickType_t w) { return xQueueSend(q, item, w); }

BaseType_t xQueueReceive(QueueHandle_t h, void* item, TickType_t wait) {
    Queue* q = (Queue*)h;
    std::unique_lock<std::mutex> lk(q->m);
    if (!waitFor(lk, q->cv, wait, [q] { return !q->items.empty(); })) return pdFAIL;
    memcpy(item, q->items.front().data(), q->size);
    q->items.pop_front();
    q->cv.notify_all();
    return pdPASS;
}

UBaseType_t uxQueueMessagesWaiting(QueueHandle_t h) {
    Queue* q = (Queue*)h;
    std::lock_guard<std::mutex> lk(q->m);
    return (UBaseType_t)q->items.size();
}

// NOSPLIT ring buffers: whole items, capacity counted in bytes plus the
// 8-byte header each item costs on the device.
RingbufHandle_t xRingbufferCreate(size_t size, int) {
    Ring* r = new Ring();
    r->cap = size;
    return r;
}
RingbufHandle_t xRingbufferCreateStatic(size_t size, int type, uint8_t*, StaticRingbuffer_t*) {
    return xRingbufferCreate(size, type);
}

static size_t itemCost(size_t n) { return ((n + 3) & ~size_t(3)) + 8; }

BaseType_t xRingbufferSendAcquire(RingbufHandle_t h, void** ptr, size_t size, TickType_t wait) {
    Ring* r = (Ring*)h;
    std::unique_lock<std::mutex> lk(r->m);
    if (!waitFor(lk, r->cv, wait, [r, size] { return r->used + itemCost(size) <= r->cap; })) return pdFALSE;
    auto* v = new std::vector<uint8_t>(size);
    r->used += itemCost(size);
    r->pending[v->data()] = v;
    *ptr = v->data();
    return pdTRUE;
}

BaseType_t xRingbufferSendComplete(RingbufHandle_t h, void* ptr) {
    Ring* r = (Ring*)h;
    std::lock_guard<std::mutex> lk(r->m);
    auto it = r->pending.find(ptr);
    if (it == r->pending.end()) return pdFALSE;
    r->ready.push_back(it->second);
    r->pending.erase(it);
    r->cv.notify_all();
    return pdTRUE;
}

BaseType_t xRingbufferSend(RingbufHandle_t h, const void* data, size_t size, TickType_t wait) {
    void* p = nullptr;
    if (xRingbufferSendAcquire(h, &p, size, wait) != pdTRUE) return pdFALSE;
    memcpy(p, data, size);
    return xRingbufferSendComplete(h, p);
}

void* xRingbufferReceive(RingbufHandle_t h, size_t* size, TickType_t wait) {
    Ring* r = (Ring*)h;
    std::unique_lock<std::mutex> lk(r->m);
    if (!waitFor(lk, r->cv, wait, [r] { return !r->ready.empty(); })) return nullptr;
    auto* v = r->ready.front();
    r->ready.pop_front();
    r->pending[v->data()] = v;   // owned until returned
    *size = v->size();
    return v->data();
}

void vRingbufferReturnItem(RingbufHandle_t h, void* item) {
    Ring* r = (Ring*)h;
    std::lock_guard<std::mutex> lk(r->m);
    auto it = r->pending.find(item);
    if (it == r->pending.end()) return;
    r->used -= itemCost(it->second->size());
    delete it->second;
    r->pending.erase(it);
    r->cv.notify_all();
}

}  // extern "C"
