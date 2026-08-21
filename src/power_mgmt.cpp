#include "power_mgmt.h"

#include <Arduino.h>
#include <esp_pm.h>
#include <esp_sleep.h>
#include <esp_err.h>
#include <driver/uart.h>

#include <esp_bt.h>

#include <atomic>

#include "ble_sensors.h"
#include "ble_server.h"
#include "diag.h"
#include "ride_recorder.h"

namespace power_mgmt {

static esp_pm_lock_handle_t s_usbLock = nullptr;
static esp_pm_lock_handle_t s_busyLock = nullptr;
static bool s_usbHeld = false;
static bool s_enabled = false;

// Bookkeeping for stateStr(): outstanding busy acquires, when the count last
// left zero, and the reasons tick() most recently held the usb lock for. The
// count is exact (atomic); heldSince is best-effort — a race can smear it by a
// call or two, which does not matter for "has this been held for 30 seconds".
static std::atomic<int> s_busyCount{0};
static volatile uint32_t s_busyHeldSinceMs = 0;   // 0 = not held
static volatile bool s_grace = false, s_serial = false;
static volatile bool s_phone = false, s_hunt = false;
// Phone connected but NOT holding sleep off (relaxed-interval experiment
// active). Not a holder — shown on the battery line as "prlx" so a sample
// can't be misread as "app wasn't connected".
static volatile bool s_phoneRelaxed = false;
// Any BLE sensor link up (holds sleep off — see tick()).
static volatile bool s_sens = false;

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

// While the companion app is connected AT A FAST INTERVAL, light sleep is held
// off.
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
// 2026-08-17: the conn-param governor (ble_server) parks an idle riding link
// at 150-300 ms, where the same 5% drift is a 10x smaller fraction of the
// window, and a 44-minute drive there recorded zero drops (with sleep held).
// So the hold was released while ble_server MEASURED the interval long.
//
// 2026-08-19: tried on the road and FALSIFIED — first light sleep killed the
// link in 4 s, three supervision timeouts in 41 s, latch tripped. The RC
// clock misses the controller's wake regardless of interval, so the interval
// was never the lever. relaxedSleepAllowed() is now false by default and the
// hold is effectively unconditional again; the machinery stays so `sleepexp
// on` can re-run the measurement in minutes after a real controller-clock fix
// (external 32 kHz crystal — lib-builder rebuild, schematic check first).

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
    bool phoneUp = ble_server::isPhoneConnected();
    // THE EXPERIMENT (2026-08-17): with the connection interval measured long
    // (>= 100 ms, governor-relaxed while riding) the controller's sleep-clock
    // drift budget is 10x what it was at the 30 ms interval that produced the
    // 2026-08-02 supervision-timeout storm — so let the CPU sleep through a
    // connected-but-idle link and find out. Three timeouts in a boot flips
    // relaxedSleepAllowed() off and this degrades to the old always-hold.
    bool phone = phoneUp && !(ble_server::linkRelaxed() &&
                              ble_server::relaxedSleepAllowed());
    s_phoneRelaxed = phoneUp && !phone;
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
    // 2026-08-21, learned on the road: an ESTABLISHED sensor link dies under
    // light sleep exactly like the phone's does. A ride ran 1h45m rock-solid
    // while the connected phone held sleep off — then the phone left, sleep
    // engaged, and the power meter dropped 40 s later, after which every
    // reconnect lived only the few seconds the hunt's own hold bought it and
    // died at supervision timeout, all the way home (connect/520 every ~30 s).
    // So the CPU only sleeps when the BLE radio is COMPLETELY quiet: no phone,
    // no sensor links, no hunt. Parked/idle — the state that actually drains
    // the battery for hours — has none of those, and keeps the full win.
    //
    // Exception, by design: a ride parked in a LONG auto-pause (2 min — see
    // ride_recorder::longAutoPaused) releases the sensor hold. The meter is
    // sleeping itself at a stop like that and the links are going away either
    // way; what matters is that the hold and the hunt come back the moment
    // the ride resumes, which they do — this flag flips false on the resume
    // tick, before the sensors have woken enough to reconnect.
    bool sensors = ble_sensors::anyConnected() &&
                   !ride_recorder::longAutoPaused();
    bool grace = millis() < USB_GRACE_MS;
    bool serial = (bool)Serial;
    bool usb = grace || serial || phone || hunting || sensors;
    s_grace = grace; s_serial = serial; s_phone = phone; s_hunt = hunting;
    s_sens = sensors;

    // A busy (bus) lock held for half a minute straight is not a transaction,
    // it is a leak — or a host copying files over MSC, which the log line lets
    // you tell apart. Either way light sleep is off and the battery line will
    // read ~60 mA high, so say so once rather than leaving a silent -175 mA
    // mystery in the logs.
    {
        uint32_t heldSince = s_busyHeldSinceMs;
        static uint32_t lastWarnMs = 0;
        if (heldSince && millis() - heldSince > 30000 &&
            millis() - lastWarnMs > 300000) {
            lastWarnMs = millis();
            diag::log("pm: bus lock held %lus straight (count %d) — light sleep "
                      "suppressed", (unsigned long)((millis() - heldSince) / 1000),
                      s_busyCount.load());
        }
    }

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
        // At most one transition logged per minute: a duty-cycled sensor hunt
        // flips this every few seconds, and logging each flip was tens of KB an
        // hour out of the diag ring. The steady state is on every battery line
        // now ("pm hunt"), so the per-flip record buys nothing.
        static uint32_t lastLogMs = 0;
        if (e == ESP_OK && (lastLogMs == 0 || millis() - lastLogMs > 60000)) {
            lastLogMs = millis();
            diag::log("pm: BT modem sleep %s (phone %s, sensor hunt %s)",
                      wantBtSleep ? "on" : "OFF",
                      s_phoneRelaxed ? "relaxed" : phoneUp ? "connected" : "gone",
                      hunting ? "ON" : "off");
        }
    }
    if (usb && !s_usbHeld) {
        esp_pm_lock_acquire(s_usbLock);
        s_usbHeld = true;
    } else if (!usb && s_usbHeld) {
        esp_pm_lock_release(s_usbLock);
        s_usbHeld = false;
    }
}

void busyAcquire() {
    if (s_busyLock) esp_pm_lock_acquire(s_busyLock);
    if (s_busyCount.fetch_add(1) == 0) s_busyHeldSinceMs = millis() | 1;
}
void busyRelease() {
    if (s_busyCount.fetch_sub(1) == 1) s_busyHeldSinceMs = 0;
    if (s_busyLock) esp_pm_lock_release(s_busyLock);
}

void stateStr(char* out, size_t n) {
    if (!s_enabled) { snprintf(out, n, "off"); return; }
    size_t p = 0;
    out[0] = 0;
    auto add = [&](const char* tok) {
        if (p < n) p += snprintf(out + p, n - p, "%s%s", p ? "+" : "", tok);
    };
    if (s_grace) add("grace");
    if (s_serial) add("serial");
    if (s_phone) add("phone");
    if (s_hunt) add("hunt");
    if (s_sens) add("sens");
    // Info, not a holder: phone attached on the relaxed link, CPU sleeping.
    if (s_phoneRelaxed) add("prlx");
    const int busy = s_busyCount.load();
    if (busy > 0) {
        char b[8];
        snprintf(b, sizeof(b), "b%d", busy);
        add(b);
    }
    if (!p) snprintf(out, n, "clear");
}

}  // namespace power_mgmt
