// Arduino and ESP-IDF on the host: the clock, the USB console (Serial), the
// GPS UART (Serial2), the BOOT button, timers, OTA and restarts.
#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <Update.h>
#include <esp_ota_ops.h>
#include <esp32-hal-tinyusb.h>
#include <soc/uart_struct.h>

#include "config.h"
#include "sim.h"
#include "sim_clock.h"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <random>
#include <thread>
#include <unistd.h>

uint32_t millis() { return sim_clock::millis(); }
uint32_t micros() { return (uint32_t)sim_clock::micros64(); }
void delay(uint32_t ms) { sim_clock::sleepMs(ms); }
void delayMicroseconds(uint32_t us) { if (us >= 1000) sim_clock::sleepMs(us / 1000); }
void yield() { std::this_thread::yield(); }

void digitalWrite(int, int) {}
int digitalRead(int pin) {
    if (pin == BOARD_BOOT_BTN) return sim::buttonDown("boot") ? LOW : HIGH;
    return HIGH;
}
extern "C" int gpio_get_level(gpio_num_t pin) { return digitalRead(pin); }

extern "C" uint32_t esp_random(void) {
    static std::mutex m;
    static std::mt19937 rng(std::random_device{}());
    std::lock_guard<std::mutex> lk(m);
    return rng();
}
extern "C" void esp_fill_random(void* buf, size_t len) {
    auto* p = (uint8_t*)buf;
    for (size_t i = 0; i < len; ++i) p[i] = (uint8_t)esp_random();
}
extern "C" int64_t esp_timer_get_time(void) { return (int64_t)sim_clock::micros64(); }
extern "C" void esp_restart(void) { sim::reboot("esp_restart", ESP_RST_SW); }
extern "C" void esp_deep_sleep_start(void) { sim::powerOff("deep sleep"); }
void usb_persist_restart(restart_type_t mode) {
    sim::reboot(mode == RESTART_BOOTLOADER ? "restart to bootloader" : "usb restart", ESP_RST_SW);
}

extern "C" const esp_app_desc_t* esp_ota_get_app_description(void) {
    static esp_app_desc_t d = [] {
        esp_app_desc_t a{};
        snprintf(a.version, sizeof a.version, "%s", FIRMWARE_VERSION);
        snprintf(a.project_name, sizeof a.project_name, "otp-sim");
        return a;
    }();
    return &d;
}

uart_dev_t UART0, UART1, UART2;
const uint32_t GPIO_PIN_MUX_REG[49] = {};
SPIClass SPI;
TwoWire Wire(0), Wire1(1);

// ---- esp_timer: one thread per started timer ------------------------------

struct esp_timer {
    esp_timer_create_args_t args;
    std::mutex m;
    std::condition_variable cv;
    uint64_t gen = 0;
};

extern "C" esp_err_t esp_timer_create(const esp_timer_create_args_t* a, esp_timer_handle_t* out) {
    auto* t = new esp_timer();
    t->args = *a;
    *out = t;
    return ESP_OK;
}

static void armTimer(esp_timer_handle_t t, uint64_t us, bool periodic) {
    uint64_t gen;
    {
        std::lock_guard<std::mutex> lk(t->m);
        gen = ++t->gen;
        t->cv.notify_all();
    }
    std::thread([t, us, periodic, gen] {
        for (;;) {
            std::unique_lock<std::mutex> lk(t->m);
            if (t->cv.wait_for(lk, sim_clock::realDuration((uint32_t)(us / 1000 ? us / 1000 : 1)),
                               [t, gen] { return t->gen != gen; }))
                return;   // stopped or re-armed
            lk.unlock();
            t->args.callback(t->args.arg);
            if (!periodic) return;
        }
    }).detach();
}

extern "C" esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us) { armTimer(t, us, false); return ESP_OK; }
extern "C" esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us) { armTimer(t, us, true); return ESP_OK; }
extern "C" esp_err_t esp_timer_stop(esp_timer_handle_t t) {
    std::lock_guard<std::mutex> lk(t->m);
    ++t->gen;
    t->cv.notify_all();
    return ESP_OK;
}
extern "C" esp_err_t esp_timer_delete(esp_timer_handle_t t) { esp_timer_stop(t); return ESP_OK; }

// ---- UARTs ------------------------------------------------------------------

struct HardwareSerial::Impl {
    std::mutex m;
    std::deque<uint8_t> rx;
    std::string lineOut;   // Serial: the console line being assembled
};

HardwareSerial::HardwareSerial(int port) : port_(port), impl_(new Impl()) {}

void HardwareSerial::begin(unsigned long baud, uint32_t, int, int, bool, unsigned long, uint8_t) {
    baud_ = baud;
}
void HardwareSerial::updateBaudRate(unsigned long baud) { baud_ = baud; }

int HardwareSerial::available() {
    std::lock_guard<std::mutex> lk(impl_->m);
    return (int)impl_->rx.size();
}
int HardwareSerial::read() {
    std::lock_guard<std::mutex> lk(impl_->m);
    if (impl_->rx.empty()) return -1;
    int c = impl_->rx.front();
    impl_->rx.pop_front();
    return c;
}
int HardwareSerial::peek() {
    std::lock_guard<std::mutex> lk(impl_->m);
    return impl_->rx.empty() ? -1 : impl_->rx.front();
}

size_t HardwareSerial::write(const uint8_t* b, size_t n) {
    if (sink) { sink(b, n); return n; }
    if (port_ != 0) return n;
    // The console: whole lines to the sim log.
    std::vector<std::string> done;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        for (size_t i = 0; i < n; ++i) {
            char c = (char)b[i];
            if (c == '\n') { done.push_back(impl_->lineOut); impl_->lineOut.clear(); }
            else if (c != '\r') impl_->lineOut += c;
        }
    }
    for (auto& l : done) sim::consoleLine(l);
    return n;
}

void HardwareSerial::sim_feed(const uint8_t* b, size_t n) {
    std::function<void(void)> cb;
    {
        std::lock_guard<std::mutex> lk(impl_->m);
        impl_->rx.insert(impl_->rx.end(), b, b + n);
        cb = onReceive_;
    }
    if (cb) cb();
}

HardwareSerial Serial(0);
HardwareSerial Serial1(1);
HardwareSerial Serial2(2);

// ---- OTA ----------------------------------------------------------------------

UpdateClass Update;

bool UpdateClass::begin(size_t size, int, int, uint8_t, const char*) {
    std::string path = sim::opts.dir + "/ota.bin";
    f_ = fopen(path.c_str(), "wb");
    size_ = size;
    written_ = 0;
    err_ = f_ ? 0 : 1;
    return f_ != nullptr;
}
size_t UpdateClass::write(uint8_t* data, size_t len) {
    if (!f_) return 0;
    size_t n = fwrite(data, 1, len, f_);
    written_ += n;
    return n;
}
bool UpdateClass::end(bool evenIfRemaining) {
    if (!f_) return false;
    fclose(f_);
    f_ = nullptr;
    bool ok = evenIfRemaining || size_ == UPDATE_SIZE_UNKNOWN || written_ == size_;
    if (!ok) err_ = 8;
    sim::consoleLine(std::string("[sim] OTA image ") + (ok ? "accepted" : "INCOMPLETE") + ": " +
                     std::to_string(written_) + " bytes");
    return ok;
}
void UpdateClass::abort() {
    if (f_) fclose(f_);
    f_ = nullptr;
}
