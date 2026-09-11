#include "gps_rx_guard.h"
#include <Arduino.h>
#include "diag.h"
#ifdef PM_GPS_RX_GUARD
#include <esp_private/pm_impl.h>
#include <esp_idf_version.h>
#if !defined(PM_BLE_XTAL) || !CONFIG_IDF_TARGET_ESP32S3 || ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(4, 4, 6)
#error "GPS RX guard requires the audited ESP32-S3 / IDF 4.4.6 MAIN_XTAL profile"
#endif
#include <esp_sleep.h>
#include <esp_timer.h>
#include <driver/gpio.h>
#include <soc/gpio_struct.h>
#include <soc/uart_struct.h>
#include "config.h"
#include "power_mgmt.h"

namespace {
DRAM_ATTR portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR bool ready = false, held = false;
DRAM_ATTR int64_t lastActivity = 0;
DRAM_ATTR uint32_t starts = 0, deferred = 0, releases = 0;
static_assert(BOARD_GPS_RXD >= 32 && BOARD_GPS_RXD < 49, "RX pin must be in GPIO.in1");
#ifdef PM_GPS_EVENT_RX
constexpr int64_t QUIET_US = 10000;
#else
constexpr int64_t QUIET_US = 50000;
#endif

bool IRAM_ATTR rxActive() {
    return !(GPIO.in1.val & (1UL << (BOARD_GPS_RXD - 32))) ||
           UART2.status.rxfifo_cnt || UART2.fsm_status.st_urx_out;
}
void IRAM_ATTR holdLocked() {
    if (!held) { held = true; ++starts; }
    lastActivity = esp_timer_get_time();
}
bool IRAM_ATTR skipSleep() { return !gps_rx_guard::beforeSleep(); }

}

void gps_rx_guard::begin() {
    esp_err_t err = ready ? ESP_OK : esp_pm_register_skip_light_sleep_callback(skipSleep);
    if (err == ESP_OK) err = gpio_wakeup_enable((gpio_num_t)BOARD_GPS_RXD, GPIO_INTR_LOW_LEVEL);
    if (err == ESP_OK) err = esp_sleep_enable_gpio_wakeup();
    if (err != ESP_OK) {
        // Setup runs under the existing boot guard; retain a permanent hold
        // before PM can enable sleep if this receive guard cannot be installed.
        power_mgmt::busyAcquire();
        diag::log("gps RX guard: setup FAILED %s; sleep held off", esp_err_to_name(err));
        return;
    }
    ready = true;
    diag::log("gps RX guard: GPIO%d low-level wake; hold through RX + %lums quiet; UART2 XTAL required", BOARD_GPS_RXD, (unsigned long)(QUIET_US / 1000));
}

bool IRAM_ATTR gps_rx_guard::beforeSleep() {
    if (!ready) return true;
    portENTER_CRITICAL(&mux);
    if (rxActive()) holdLocked();
    bool active = held;
    if (active) ++deferred;
    portEXIT_CRITICAL(&mux);
    return !active;
}
void IRAM_ATTR gps_rx_guard::afterSleep() {
    if (!ready) return;
    portENTER_CRITICAL(&mux);
    // Only update internal state here. Do not acquire an ESP PM lock from
    // this callback: IDF already holds its switch lock. The registered skip
    // callback suppresses the next idle sleep until the GPS task sees quiet.
    // GPIO wake may have gone high by the time sleep returns. Retain the
    // wake cause as well as the FIFO/FSM state to catch that first byte.
    if (esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_GPIO || rxActive()) holdLocked();
    portEXIT_CRITICAL(&mux);
}
void gps_rx_guard::tick(bool receivedBytes) {
    if (!ready) return;
    portENTER_CRITICAL(&mux);
    if (receivedBytes || rxActive()) holdLocked();
    else if (held && esp_timer_get_time() - lastActivity >= QUIET_US) {
        held = false; ++releases;
    }
    portEXIT_CRITICAL(&mux);
}
uint32_t gps_rx_guard::waitMs() {
    portENTER_CRITICAL(&mux);
    bool active = held;
    portEXIT_CRITICAL(&mux);
    return active ? QUIET_US / 1000 : 1000;
}
void gps_rx_guard::report() {
    portENTER_CRITICAL(&mux);
    bool isHeld = held; uint32_t a = starts, d = deferred, r = releases;
    portEXIT_CRITICAL(&mux);
    diag::log("gps RX guard: ready=%d held=%d bursts=%lu deferred=%lu releases=%lu fifo=%u rx_fsm=%u",
              ready, isHeld, (unsigned long)a, (unsigned long)d, (unsigned long)r,
              UART2.status.rxfifo_cnt, UART2.fsm_status.st_urx_out);
}
#else
void gps_rx_guard::begin() {}
void gps_rx_guard::tick(bool) {}
bool gps_rx_guard::beforeSleep() { return true; }
void gps_rx_guard::afterSleep() {}
void gps_rx_guard::report() {}
uint32_t gps_rx_guard::waitMs() { return 50; }
#endif
