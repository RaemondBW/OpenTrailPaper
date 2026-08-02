#include "power_mgmt.h"

#include <Arduino.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/uart.h>

#include "diag.h"

namespace power_mgmt {

static esp_pm_lock_handle_t s_usbLock = nullptr;
static esp_pm_lock_handle_t s_busyLock = nullptr;
static bool s_usbHeld = false;
static bool s_enabled = false;

bool begin() {
    // Light sleep ONLY — no dynamic frequency scaling. min == max == 240 MHz
    // keeps the 80 MHz octal (OPI) PSRAM's timing valid at all times: the PLL is
    // fully restored to 240 MHz before any post-wake code touches PSRAM, so the
    // SoC still power-gates between the ~1 Hz workloads without the reduced-clock
    // instability that boot-looped the OPI PSRAM at 160 MHz (see main.cpp setup).
    // BISECTION SWITCH. Build with -DPM_LIGHT_SLEEP=0 to link and configure the
    // whole PM stack — locks, tickless-idle-capable FreeRTOS, PM-aware drivers —
    // but never actually light-sleep. That separates the two things a PM build
    // changes at once:
    //
    //   SD works with PM_LIGHT_SLEEP=0, fails with 1  -> light sleep is landing
    //       mid-SPI-transaction. Expected, because sd_diskio.cpp drives the card
    //       through Arduino's SPIClass (esp32-hal-spi.c), which contains ZERO
    //       references to esp_pm_lock, while IDF's own spi_master in libdriver.a
    //       has 32. The Arduino path is simply not PM-aware.
    //   SD fails both ways -> it is the rebuilt libraries or their config, not
    //       sleep, and no amount of PM-lock work in our code will help.
    //
    // Costs nothing to run and it is the only way to tell those apart.
#ifndef PM_LIGHT_SLEEP
#define PM_LIGHT_SLEEP 1
#endif
    esp_pm_config_esp32s3_t cfg = {};
    cfg.max_freq_mhz = 240;
    cfg.min_freq_mhz = 240;
    cfg.light_sleep_enable = (PM_LIGHT_SLEEP != 0);

    esp_err_t err = esp_pm_configure(&cfg);
    s_enabled = (err == ESP_OK);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        diag::log("pm: light sleep UNAVAILABLE — framework lacks CONFIG_PM_ENABLE"
                  "/TICKLESS_IDLE (rebuild required; see CPU_SLEEP_SPIKE.md)");
        return false;
    }
    diag::log("pm: esp_pm_configure(min=max=240, light_sleep=%d) -> %s",
              PM_LIGHT_SLEEP != 0, esp_err_to_name(err));

    // Keep the USB-CDC serial console alive while plugged in. Light sleep gates
    // the USB-OTG PHY, dropping the CDC link; hold a no-light-sleep lock whenever
    // the host has the port open. On battery (USB absent) the lock stays released
    // so the CPU can sleep. (Returns NOT_SUPPORTED and a null handle on a stock
    // framework — tick() then no-ops.)
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "usbcdc", &s_usbLock) != ESP_OK) {
        s_usbLock = nullptr;
    } else {
        // START HELD. Light sleep is armed by the esp_pm_configure() above, but
        // the first tick() does not run until loop() starts — several hundred ms
        // later, after the remaining setup() work. A sleep in that gap gates the
        // USB-OTG PHY and kills the CDC link that has only just enumerated, and
        // TinyUSB does not bring it back. Acquiring here means the SoC cannot
        // sleep until tick() decides it may.
        esp_pm_lock_acquire(s_usbLock);
        s_usbHeld = true;
    }

    // Bus-transaction lock — see busyAcquire() in the header for why the SD
    // card specifically needs one. Created unheld.
    if (esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "bus", &s_busyLock) != ESP_OK)
        s_busyLock = nullptr;

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

// Grace window after boot during which light sleep is held off unconditionally.
//
// LOAD-BEARING — without it there is no console and no OTG flashing, ever.
// (bool)Serial is only true once a host has OPENED the CDC port, but light
// sleep gates the USB-OTG PHY and drops the link before a host gets the chance:
// the CPU sleeps ~1 s after boot, USB dies, the port becomes unopenable, so
// `connected` can never become true and the lock is never taken. The condition
// this guard depends on is destroyed by the thing it is guarding against.
//
// Observed on the first PM-enabled build (2026-07-31): the device booted fine
// and enumerated its CDC port, then went permanently silent within a second.
// 30 s is enough to attach a monitor or start an upload after a reset; the cost
// is that the device cannot sleep for the first 30 s of each boot.
static constexpr uint32_t USB_GRACE_MS = 30000;

void tick() {
    if (!s_usbLock) return;
    // Held open during the grace window, then only while a host is attached.
    bool usb = (millis() < USB_GRACE_MS) || (bool)Serial;
    if (usb && !s_usbHeld) {
        esp_pm_lock_acquire(s_usbLock);
        s_usbHeld = true;
    } else if (!usb && s_usbHeld) {
        esp_pm_lock_release(s_usbLock);
        s_usbHeld = false;
    }
}

void busyAcquire() { if (s_busyLock) esp_pm_lock_acquire(s_busyLock); }
void busyRelease() { if (s_busyLock) esp_pm_lock_release(s_busyLock); }

}  // namespace power_mgmt
