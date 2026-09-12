#include <Arduino.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include "power_mgmt.h"
#include "gps_rx_guard.h"

namespace {
DRAM_ATTR portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
DRAM_ATTR uint32_t successes = 0, failures = 0;
DRAM_ATTR uint64_t elapsedUs = 0;
}

extern "C" esp_err_t __real_esp_light_sleep_start();
// Called by IDF's tickless-idle path, sometimes with interrupts disabled.
// IRAM/internal data only; no allocation, logging, SD, or FreeRTOS waits.
extern "C" esp_err_t IRAM_ATTR __wrap_esp_light_sleep_start() {
#ifdef PM_GPS_RX_GUARD
    if (!gps_rx_guard::beforeSleep()) return ESP_ERR_SLEEP_REJECT;
#endif
    int64_t start = esp_timer_get_time();
    esp_err_t result = __real_esp_light_sleep_start();
    uint64_t elapsed = esp_timer_get_time() - start;
#ifdef PM_GPS_RX_GUARD
    gps_rx_guard::afterSleep();
#endif
    portENTER_CRITICAL(&mux);
    if (result == ESP_OK) { ++successes; elapsedUs += elapsed; }
    else ++failures;
    portEXIT_CRITICAL(&mux);
    return result;
}

void power_mgmt::sleepStats(uint32_t& ok, uint32_t& rejected, uint64_t& us) {
    portENTER_CRITICAL(&mux);
    ok = successes; rejected = failures; us = elapsedUs;
    portEXIT_CRITICAL(&mux);
}
