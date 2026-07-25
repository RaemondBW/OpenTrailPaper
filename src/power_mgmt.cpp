#include "power_mgmt.h"

#include <Arduino.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/uart.h>

#include "diag.h"

namespace power_mgmt {

static esp_pm_lock_handle_t s_usbLock = nullptr;
static bool s_usbHeld = false;
static bool s_enabled = false;

bool begin() {
    // Light sleep ONLY — no dynamic frequency scaling. min == max == 240 MHz
    // keeps the 80 MHz octal (OPI) PSRAM's timing valid at all times: the PLL is
    // fully restored to 240 MHz before any post-wake code touches PSRAM, so the
    // SoC still power-gates between the ~1 Hz workloads without the reduced-clock
    // instability that boot-looped the OPI PSRAM at 160 MHz (see main.cpp setup).
    esp_pm_config_esp32s3_t cfg = {};
    cfg.max_freq_mhz = 240;
    cfg.min_freq_mhz = 240;
    cfg.light_sleep_enable = true;

    esp_err_t err = esp_pm_configure(&cfg);
    s_enabled = (err == ESP_OK);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        diag::log("pm: light sleep UNAVAILABLE — framework lacks CONFIG_PM_ENABLE"
                  "/TICKLESS_IDLE (rebuild required; see CPU_SLEEP_SPIKE.md)");
        return false;
    }
    diag::log("pm: esp_pm_configure(min=max=240, light_sleep=1) -> %s",
              esp_err_to_name(err));

    // Keep the USB-CDC serial console alive while plugged in. Light sleep gates
    // the USB-OTG PHY, dropping the CDC link; hold a no-light-sleep lock whenever
    // the host has the port open. On battery (USB absent) the lock stays released
    // so the CPU can sleep. (Returns NOT_SUPPORTED and a null handle on a stock
    // framework — tick() then no-ops.)
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usbcdc", &s_usbLock) != ESP_OK)
        s_usbLock = nullptr;

#ifdef PM_GPS_UART_WAKEUP
    // OPTIONAL belt-and-suspenders against NMEA loss: wake on GPS RX so the
    // 128-byte UART hardware FIFO is drained before it can overflow (~133 ms at
    // 9600 baud). The S3 can only wake from UART0/UART1, but SerialGPS currently
    // lives on UART2 — move it to Serial1 in gps_service.cpp to use this. NOT
    // required as long as a task polls faster than the FIFO fill time: the GPS
    // task's own 50 ms poll (gps_service.cpp task loop) already bounds every
    // sleep to <= ~50 ms (~48 bytes), well under the 128-byte FIFO.
    uart_set_wakeup_threshold((uart_port_t)PM_GPS_UART_WAKEUP, 3);
    esp_sleep_enable_uart_wakeup(PM_GPS_UART_WAKEUP);
#endif
    return s_enabled;
}

void tick() {
    if (!s_usbLock) return;
    bool usb = (bool)Serial;                     // host holds the CDC port open?
    if (usb && !s_usbHeld) {
        esp_pm_lock_acquire(s_usbLock);
        s_usbHeld = true;
    } else if (!usb && s_usbHeld) {
        esp_pm_lock_release(s_usbLock);
        s_usbHeld = false;
    }
}

}  // namespace power_mgmt
