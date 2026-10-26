// Host FreeRTOS: tasks are threads, semaphores are condition variables, ticks
// are milliseconds of the sim clock (sim_clock.h). Only what src/ uses.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int BaseType_t;
typedef unsigned UBaseType_t;
typedef uint32_t TickType_t;
typedef void* TaskHandle_t;
typedef void* SemaphoreHandle_t;
typedef void* QueueHandle_t;
typedef void* RingbufHandle_t;
typedef void (*TaskFunction_t)(void*);
typedef struct { int dummy; } StaticRingbuffer_t;
typedef struct { int owner; int count; } portMUX_TYPE;

#define pdTRUE 1
#define pdFALSE 0
#define pdPASS 1
#define pdFAIL 0
#define portMAX_DELAY 0xFFFFFFFFu
#define portTICK_PERIOD_MS 1
#define configTICK_RATE_HZ 1000
#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))
#define portMUX_INITIALIZER_UNLOCKED {0, 0}
#define tskNO_AFFINITY 0x7FFFFFFF
#define RINGBUF_TYPE_NOSPLIT 0
#define RINGBUF_TYPE_BYTEBUF 2

void sim_critical_enter(void);
void sim_critical_exit(void);
#define portENTER_CRITICAL(m) sim_critical_enter()
#define portEXIT_CRITICAL(m) sim_critical_exit()
#define portENTER_CRITICAL_ISR(m) sim_critical_enter()
#define portEXIT_CRITICAL_ISR(m) sim_critical_exit()
#define taskENTER_CRITICAL(m) sim_critical_enter()
#define taskEXIT_CRITICAL(m) sim_critical_exit()
#define portYIELD_FROM_ISR(...) do {} while (0)
#define xPortInIsrContext() 0
#define xPortGetCoreID() 1

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char* name, uint32_t stack,
                                   void* arg, UBaseType_t prio, TaskHandle_t* out, BaseType_t core);
BaseType_t xTaskCreate(TaskFunction_t fn, const char* name, uint32_t stack, void* arg,
                       UBaseType_t prio, TaskHandle_t* out);
void vTaskDelay(TickType_t ticks);
void vTaskDelete(TaskHandle_t t);
TickType_t xTaskGetTickCount(void);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t);
BaseType_t xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t wait);
void vTaskNotifyGiveFromISR(TaskHandle_t t, BaseType_t* woken);

SemaphoreHandle_t xSemaphoreCreateMutex(void);
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex(void);
SemaphoreHandle_t xSemaphoreCreateBinary(void);
SemaphoreHandle_t xSemaphoreCreateCounting(UBaseType_t max, UBaseType_t initial);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
BaseType_t xSemaphoreTakeRecursive(SemaphoreHandle_t s, TickType_t wait);
BaseType_t xSemaphoreGiveRecursive(SemaphoreHandle_t s);
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s, BaseType_t* woken);
void vSemaphoreDelete(SemaphoreHandle_t s);

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t itemSize);
BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t wait);
BaseType_t xQueueSendToBack(QueueHandle_t q, const void* item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t wait);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);

RingbufHandle_t xRingbufferCreate(size_t size, int type);
RingbufHandle_t xRingbufferCreateStatic(size_t size, int type, uint8_t* storage, StaticRingbuffer_t* rb);
BaseType_t xRingbufferSend(RingbufHandle_t rb, const void* data, size_t size, TickType_t wait);
BaseType_t xRingbufferSendAcquire(RingbufHandle_t rb, void** ptr, size_t size, TickType_t wait);
BaseType_t xRingbufferSendComplete(RingbufHandle_t rb, void* ptr);
void* xRingbufferReceive(RingbufHandle_t rb, size_t* size, TickType_t wait);
void vRingbufferReturnItem(RingbufHandle_t rb, void* item);

#ifdef __cplusplus
}
#endif
