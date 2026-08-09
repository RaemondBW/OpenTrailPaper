#include "power_mgmt.h"

#include <Arduino.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/uart.h>

#include <esp_bt.h>

#include "ble_sensors.h"
#include "ble_server.h"
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
                  "/TICKLESS_IDLE (rebuild required; see investigations/archive/cpu-sleep-spike.md)");
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

// While the companion app is connected, light sleep is held off entirely.
//
// WHY: the BT controller sleeps between connection events and wakes on the RTC
// slow clock, which on this board is the internal 150 kHz RC — it drifts ~5%
// (sdkconfig.defaults.pm says so, and calls it "slightly widens BLE wake
// windows"). At a 30 ms connection interval that is ~1.5 ms of error per event,
// enough to miss the master's anchor point, drop packets, and hit supervision
// timeout. Measured 2026-08-02: 1231 phone disconnects, ALL of them with light
// sleep on, none with it off — every one HCI 0x08, supervision timeout — which
// made map transfers over BLE practically impossible.
//
// The phone is only connected while the rider is actually using the app, so
// suppressing sleep for that window costs little: the long tail of a ride has
// no phone attached and still sleeps. The real fix is a stable low-power clock
// for the controller (CONFIG_RTC_CLK_SRC_EXT_CRYS + BT_CTRL_LPCLK_SEL_EXT_32K_XTAL
// if the board wires a 32.768 kHz crystal, else BT_CTRL_MODEM_SLEEP=n), but both
// need a lib-builder rebuild; this needs none and can ship today.

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
    // Held during the boot grace window, while a serial host is attached, and
    // for as long as the phone is connected (see the note above).
    // Ask ble_server DIRECTLY, not via g_state. The shared state's copy is
    // written by the UI task at 1 Hz and read here at 1 Hz, so it lagged the
    // actual connection by up to two seconds — long enough for the link to time
    // out before sleep was ever suppressed.
    bool phone = ble_server::isPhoneConnected();
    // The SAME suppression the phone gets, for the same reason, extended to the
    // sensor hunt.
    //
    // Sleep does not just cost packets on an established link — it costs
    // DISCOVERY. Scanning only hears an advertisement if the controller is awake
    // in its scan window, and with BT modem sleep waking on that ~5%-drift
    // 150 kHz RC the windows land off-target, so a sensor that dropped out mid
    // ride (a power meter that slept through a coffee stop, a strap that lost
    // contact) advertised into a radio that was not listening and never came
    // back. Symptom from the road, 2026-08-08: sensors would not reconnect after
    // a break UNLESS the phone app was connected — because a connected phone is
    // exactly what used to hold this off.
    //
    // Costs nothing in the steady state: radioBusy() is only true while a PAIRED
    // sensor is missing and the device is actually looking for it. Once
    // everything is connected the hunt stops and the CPU sleeps again.
    bool hunting = ble_sensors::radioBusy();
    bool usb = (millis() < USB_GRACE_MS) || (bool)Serial || phone || hunting;

    // Suppressing CPU light sleep is NOT enough on its own. The BT controller
    // has its own modem sleep, and with CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW it
    // wakes for each connection event on the internal 150 kHz RC — the ~5%-drift
    // clock. It widens its receive window by the sleep-clock accuracy it
    // advertised, and that RC cannot hold to it, so events are missed and the
    // link dies of supervision timeout no matter what the CPU is doing.
    // Stop the controller sleeping while the phone is attached; let it sleep
    // again when it goes away, since that is the state a ride spends its time in.
    static int btSleep = -1;                 // -1 unknown, 0 disabled, 1 enabled
    int wantBtSleep = (phone || hunting) ? 0 : 1;
    if (btSleep != wantBtSleep) {
        esp_err_t e = wantBtSleep ? esp_bt_sleep_enable() : esp_bt_sleep_disable();
        if (e == ESP_OK || e == ESP_ERR_INVALID_STATE) btSleep = wantBtSleep;
        if (e == ESP_OK)
            diag::log("pm: BT modem sleep %s (phone %s, sensor hunt %s)",
                      wantBtSleep ? "on" : "OFF", phone ? "connected" : "gone",
                      hunting ? "ON" : "off");
    }
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
