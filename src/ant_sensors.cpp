#include "ant_sensors.h"

#include <Arduino.h>
#include <cstring>

#include "ant_node.h"
#include "ant_phy_shockburst.h"
#include "ble_sensors.h"
#include "diag.h"

namespace {

using ble_sensors::KIND_HR;
using ble_sensors::KIND_POWER;
using ble_sensors::KIND_CSC;

// A fresh pair search gives up after this long (ANT search timeout is in
// 2.5 s units). Long enough to wake a sleeping meter with a pedal stroke.
// A wildcard search hears a sensor at only a fraction of its page rate (the
// four-byte sync word has to swallow the one-byte ANT preamble), so give it
// time: three minutes, and the UI tells the rider to bring the sensor close.
constexpr uint8_t  PAIR_TIMEOUT_UNITS = 72;          // 180 s
constexpr uint32_t PAIR_TIMEOUT_MS    = PAIR_TIMEOUT_UNITS * 2500u + 5000u;
// A paired master heard this close is "on my bike". The library only applies
// it to a FRESH search (pairing), never to reacquiring a remembered device.
constexpr int8_t   PAIR_PROXIMITY_DBM = -70;
static int8_t s_prox = PAIR_PROXIMITY_DBM;   // console `ant prox <dBm>` adjusts it
// A tracked channel whose pages stop is dropped after this long: same age at
// which ble_sensors' staleness sweep blanks the value and the dashboard greys
// the field, so the two paths agree on when a sensor is gone.
constexpr uint32_t STALE_MS = 15000;

struct Kind {
    const char* name;
    uint8_t     devType;      // type a fresh pair search looks for
    uint16_t    period;
    uint8_t     altType;      // a second type that serves this kind (0 = none)
    // Written on the ANT task (plain stores), read anywhere.
    volatile bool     tracking = false;   // pages arriving
    volatile bool     pairing  = false;   // fresh search open
    volatile int8_t   rssi     = 0;
    volatile uint32_t pages    = 0;
    volatile uint32_t lastPageMs = 0;
    uint32_t          pairStartMs = 0;
    // Feed-task view: whether RideState currently says "connected" for us.
    bool published = false;
};

constexpr uint16_t ANTPLUS_PERIOD_BIKE_CADENCE = 8102;   // cadence-only sensors (type 122)
Kind kinds[ant_sensors::KIND_COUNT] = {
    {"HR",      ANTPLUS_DEVTYPE_HRM,         ANTPLUS_PERIOD_HRM,         0},
    {"Power",   ANTPLUS_DEVTYPE_BIKE_POWER,  ANTPLUS_PERIOD_BIKE_POWER,  0},
    {"Cadence", ANTPLUS_DEVTYPE_BIKE_SPDCAD, ANTPLUS_PERIOD_BIKE_SPDCAD, ANTPLUS_DEVTYPE_BIKE_CADENCE},
};
constexpr uint8_t SCAN_CH = 3;               // channels 0-2 are the kinds

uint16_t periodForType(uint8_t t) {
    switch (t) {
        case ANTPLUS_DEVTYPE_HRM:          return ANTPLUS_PERIOD_HRM;
        case ANTPLUS_DEVTYPE_BIKE_POWER:   return ANTPLUS_PERIOD_BIKE_POWER;
        case ANTPLUS_DEVTYPE_BIKE_SPDCAD:  return ANTPLUS_PERIOD_BIKE_SPDCAD;
        case ANTPLUS_DEVTYPE_BIKE_CADENCE: return ANTPLUS_PERIOD_BIKE_CADENCE;
        default:                           return ANTPLUS_PERIOD_HRM;
    }
}

// Channel number == kind index.
ant_node_t node;
bool nodeUp = false;
// The remembered-device table, mirrored from the library's NVS store so the
// UI can answer "is HR paired?" while the node is down (ant_node_known_device
// needs a running node).
ant_known_t known;

// Everything that touches RideState or the log happens on the feed task; the
// ANT callbacks (high-priority ANT task, node lock held) only enqueue.
enum EvType : uint8_t { EV_PAGE, EV_DROP, EV_PAIRED, EV_TIMEOUT, EV_START_FAIL };
struct Ev {
    uint8_t  type;
    uint8_t  kind;
    int8_t   rssi;
    uint8_t  status;      // EV_START_FAIL: ant_espphy_status_t
    uint16_t deviceNum;
    uint8_t  page[8];
};
QueueHandle_t evq = nullptr;

void post(const Ev& e) {
    if (evq) xQueueSend(evq, &e, 0);
}

int kindOfChannel(uint8_t ch) { return ch < ant_sensors::KIND_COUNT ? ch : -1; }

// --- ANT task callbacks -----------------------------------------------------

void onData(ant_node_t*, const ant_node_rx_t* rx, const uint8_t page[8], void*) {
    int k = kindOfChannel(rx->channel);
    if (k < 0) return;
    Kind& K = kinds[k];
    K.tracking = true;
    K.rssi = rx->rssi;
    K.pages = K.pages + 1;
    K.lastPageMs = millis();
    Ev e{};
    e.type = EV_PAGE;
    e.kind = (uint8_t)k;
    e.rssi = rx->rssi;
    e.deviceNum = rx->device_num;
    e.status = rx->device_type;      // EV_PAGE: the sending device's type
    memcpy(e.page, page, 8);
    post(e);
}

void onEvent(ant_node_t*, uint8_t ch, uint8_t event, void*) {
    int k = kindOfChannel(ch);
    if (k < 0) return;
    Kind& K = kinds[k];
    Ev e{};
    e.kind = (uint8_t)k;
    switch (event) {
        case ANT_EVENT_RX_FAIL_GO_TO_SEARCH:
            // CHANNEL_CLOSED is deliberately not a drop: we only close channels
            // ourselves (a node stop for a BLE hunt, forget, re-pair), and a
            // stop-and-reopen on the remembered id must not blank the field
            // in between. The feed task's staleness check catches a sensor
            // that really is gone.
            if (!K.tracking) return;
            K.tracking = false;
            e.type = EV_DROP;
            post(e);
            break;
        case ANT_EVENT_RX_SEARCH_TIMEOUT:
            // Only a pair search has a timeout; a remembered device is waited
            // for indefinitely (the arbiter decides when to stop looking).
            if (!K.pairing) return;
            K.pairing = false;
            e.type = EV_TIMEOUT;
            post(e);
            break;
        default:
            break;
    }
}

void onPaired(ant_node_t*, uint8_t ch, const ant_node_device_t* dev, bool remembered, void*) {
    int k = kindOfChannel(ch);
    if (k < 0) return;
    Kind& K = kinds[k];
    if (remembered) ant_known_put(&known, dev);   // mirror the table
    K.pairing = false;
    Ev e{};
    e.type = EV_PAIRED;
    e.kind = (uint8_t)k;
    e.deviceNum = dev->device_num;
    e.status = remembered;
    post(e);
}

// --- channels ---------------------------------------------------------------

// The remembered device serving a kind, if any (either of its types).
const ant_known_device_t* knownFor(int k) {
    const ant_known_device_t* d = ant_known_find(&known, kinds[k].devType);
    if (!d && kinds[k].altType) d = ant_known_find(&known, kinds[k].altType);
    return d;
}
bool isPaired(int k) { return knownFor(k) != nullptr; }

void openPairSearch(int k) {
    Kind& K = kinds[k];
    ant_node_close(&node, (uint8_t)k);   // harmless when unassigned
    ant_node_channel_cfg_t c{};
    c.type = ANT_CHANNEL_TYPE_SLAVE_RX;
    c.device_type = K.devType;
    c.period = K.period;
    c.search_timeout = PAIR_TIMEOUT_UNITS;
    c.flags = ANT_NODE_CH_IGNORE_KNOWN;
    uint8_t r = ant_node_open(&node, (uint8_t)k, &c);
    if (r != ANT_RESPONSE_NO_ERROR) {
        K.pairing = false;
        Serial.printf("[ant] %s pair search failed to open (0x%02x)\n", K.name, r);
    }
}

void openPaired(int k) {
    Kind& K = kinds[k];
    // Open on the remembered device's EXPLICIT id. Never a wildcard here: a
    // device number of 0 would search and adopt whatever sensor is nearest,
    // which is the one thing the pairing policy forbids.
    const ant_known_device_t* d = knownFor(k);
    if (!d) return;
    ant_node_channel_cfg_t c{};
    c.type = ANT_CHANNEL_TYPE_SLAVE_RX;
    c.device_num = d->device_num;
    c.device_type = d->device_type;
    c.trans_type = d->trans_type;
    c.period = periodForType(d->device_type);
    c.flags = ANT_NODE_CH_NO_REMEMBER;
    uint8_t r = ant_node_open(&node, (uint8_t)k, &c);
    if (r != ANT_RESPONSE_NO_ERROR)
        Serial.printf("[ant] %s channel failed to open (0x%02x)\n", K.name, r);
}

// --- scanning ---------------------------------------------------------------
volatile bool s_scan = false;       // explicit request
volatile bool s_listScan = false;   // a sensors list is open (arbiter)
inline bool scanActive() { return s_scan || s_listScan; }
SemaphoreHandle_t candMutex = nullptr;
constexpr int MAX_CANDS = 12;
ant_sensors::Candidate cands[MAX_CANDS];
int candN = 0;

void openScan() {
    ant_node_close(&node, SCAN_CH);
    ant_node_channel_cfg_t c{};
    c.type = ANT_CHANNEL_TYPE_SLAVE_RX;   // device 0 / type 0 / trans 0: any ANT+ master
    c.period = ANTPLUS_PERIOD_HRM;
    c.flags = ANT_NODE_CH_IGNORE_KNOWN | ANT_NODE_CH_NO_REMEMBER;
    c.proximity_rssi = 127;              // no frame is ever that strong: never acquire
    uint8_t r = ant_node_open(&node, SCAN_CH, &c);
    if (r != ANT_RESPONSE_NO_ERROR) Serial.printf("[ant] scan channel failed to open (0x%02x)\n", r);
    node.phy.tap_on = true;              // the tap ring is how the scan sees frames
}

void noteScanFrame(const ant_espphy_rx_t& f) {
    // Only a frame whose ANT CRC checks: the receive path can splice a stale
    // header onto a fresh payload, which would invent devices.
    if (f.len != 17 || !ant_sb_verify_frame(f.body, f.len, 6, 9, nullptr)) return;
    uint16_t num = (uint16_t)((f.body[2] << 8) | f.body[3]);
    uint8_t type = (uint8_t)(f.body[4] & 0x7F);
    uint8_t trans = f.body[5];
    if (!num || !type) return;
    xSemaphoreTake(candMutex, portMAX_DELAY);
    int slot = -1;
    for (int i = 0; i < candN; ++i)
        if (cands[i].devType == type && cands[i].devNum == num) { slot = i; break; }
    if (slot < 0) {
        if (candN < MAX_CANDS) slot = candN++;
        else {
            slot = 0;   // evict the stalest
            for (int i = 1; i < candN; ++i)
                if ((int32_t)(cands[i].lastMs - cands[slot].lastMs) < 0) slot = i;
        }
        cands[slot] = ant_sensors::Candidate{};
        cands[slot].devType = type;
        cands[slot].devNum = num;
        cands[slot].rssi = f.rssi;
    }
    ant_sensors::Candidate& c = cands[slot];
    c.trans = trans;
    if (f.rssi > c.rssi || millis() - c.lastMs > 10000) c.rssi = f.rssi;
    c.lastMs = millis();
    c.frames++;
    xSemaphoreGive(candMutex);
}

void openChannels() {
    for (int k = 0; k < ant_sensors::KIND_COUNT; ++k) {
        if (kinds[k].pairing) openPairSearch(k);
        else if (isPaired(k)) openPaired(k);
    }
    if (scanActive()) openScan();
}

// --- feed task: pages -> RideState ------------------------------------------

ble_sensors::CrankState antCrank;      // spd/cad crank
uint16_t lastWheelRevs = 0;
bool haveWheelRevs = false;
uint32_t lastRawLogMs = 0;

void markConnected(int k, bool on) {
    Kind& K = kinds[k];
    if (K.published == on) return;
    K.published = on;
    if (on) {
        ble_sensors::feedConnected(k, true);
    } else {
        // Do not blank a kind the BLE path is still serving.
        if (!ble_sensors::kindConnected(k)) ble_sensors::feedConnected(k, false);
    }
}

void feedPage(const Ev& e) {
    int k = e.kind;
    markConnected(k, true);

    // The raw page once a minute per kind, throttled like ble_sensors' hr/pwr
    // lines and for the same reason (the diag ring is small).
    if (millis() - lastRawLogMs > 60000) {
        lastRawLogMs = millis();
        diag::log("ant %s #%u rssi %d: %02X %02X %02X %02X %02X %02X %02X %02X",
                  kinds[k].name, e.deviceNum, e.rssi, e.page[0], e.page[1], e.page[2],
                  e.page[3], e.page[4], e.page[5], e.page[6], e.page[7]);
    }

    switch (k) {
        case KIND_HR: {
            antplus_hrm_data_t hr;
            if (!antplus_hrm_decode(e.page, &hr) || hr.computed_heart_rate == 0) return;
            ble_sensors::feedHr(hr.computed_heart_rate);
            break;
        }
        case KIND_POWER: {
            // Only the standard power-only page (0x10) carries watts; the
            // others (calibration, manufacturer, battery) return false.
            antplus_power_data_t pw;
            if (!antplus_power_decode(e.page, &pw)) return;
            const bool hasCrank = pw.instantaneous_cadence != 0xFF;
            uint8_t cad = hasCrank ? pw.instantaneous_cadence : 0xFF;
            ble_sensors::feedPower(pw.instantaneous_power, hasCrank, cad,
                                   /*crankStopped=*/hasCrank && cad == 0);
            break;
        }
        case KIND_CSC: {
            antplus_spdcad_data_t sc;
            if (e.status == ANTPLUS_DEVTYPE_BIKE_CADENCE) {
                // Cadence-only profile: page 0's event time and revolution
                // count sit in bytes 4-7; there is no wheel half.
                sc.cadence_event_time = (uint16_t)(e.page[4] | (e.page[5] << 8));
                sc.cumulative_cadence = (uint16_t)(e.page[6] | (e.page[7] << 8));
                sc.speed_event_time = sc.cumulative_speed = 0;
            } else if (!antplus_spdcad_decode(e.page, &sc)) return;
            // Same 1/1024 s event clock and 16-bit counters as BLE CSC, so the
            // crank maths is shared. A sensor repeats the previous page until
            // the next event, which the helper reads as dTime == 0 -> no news.
            uint8_t cad = ble_sensors::cadenceFromCrank(antCrank, sc.cumulative_cadence,
                                                        sc.cadence_event_time);
            if (cad != 0xFF) ble_sensors::feedCadence(cad);
            if (e.status != ANTPLUS_DEVTYPE_BIKE_CADENCE) {
                if (haveWheelRevs && sc.cumulative_speed != lastWheelRevs)
                    ble_sensors::feedWheelMove();
                lastWheelRevs = sc.cumulative_speed;
                haveWheelRevs = true;
            }
            break;
        }
        default:
            break;
    }
}

volatile int tapRemaining = 0;
// Tally mode (tapRemaining < 0): count every decoded frame by ANT device type
// instead of printing it. Type 0xFF bucket = frames too short to carry one.
struct TypeTally { uint32_t n; uint32_t matched; int8_t bestRssi; };
static TypeTally tally[256];
static uint32_t tallyTotal = 0;

void drainTap() {
    if (!nodeUp) return;
    ant_espphy_rx_t f;
    bool matched;
    while (scanActive() && tapRemaining == 0 && ant_espphy_tap_poll(&node.phy, &f, &matched))
        noteScanFrame(f);
    while (tapRemaining < 0 && ant_espphy_tap_poll(&node.phy, &f, &matched)) {
        uint8_t ty = f.len >= 5 ? f.body[4] : 0xFF;
        TypeTally& t = tally[ty];
        if (!t.n || f.rssi > t.bestRssi) t.bestRssi = f.rssi;
        t.n++;
        if (matched) t.matched++;
        tallyTotal++;
    }
    while (tapRemaining > 0 && ant_espphy_tap_poll(&node.phy, &f, &matched)) {
        tapRemaining = tapRemaining - 1;
        char hex[3 * ANT_SB_FRAME_MAX + 1];
        int p = 0;
        for (int i = 0; i < f.len && i < (int)ANT_SB_FRAME_MAX; ++i)
            p += snprintf(hex + p, sizeof(hex) - p, "%02x ", f.body[i]);
        Serial.printf("[ant tap] len=%u rssi=%d %s: %s\n", f.len, f.rssi,
                      matched ? "MATCH" : "     ", hex);
        if (tapRemaining == 0) node.phy.tap_on = false;
    }
}

void feedTask(void*) {
    for (;;) {
        Ev e;
        drainTap();
        if (xQueueReceive(evq, &e, pdMS_TO_TICKS((tapRemaining != 0 || scanActive()) ? 20 : 1000)) == pdTRUE) {
            Kind& K = kinds[e.kind];
            switch (e.type) {
                case EV_PAGE:
                    feedPage(e);
                    break;
                case EV_DROP:
                    diag::log("ant %s lost (back to search)", K.name);
                    markConnected(e.kind, false);
                    break;
                case EV_PAIRED:
                    diag::log("ant %s %s device %u", K.name,
                              e.status ? "paired" : "reconnected", e.deviceNum);
                    Serial.printf("[ant] %s %s #%u\n", K.name,
                                  e.status ? "paired" : "reconnected", e.deviceNum);
                    break;
                case EV_TIMEOUT:
                    diag::log("ant %s pair search timed out", K.name);
                    Serial.printf("[ant] %s: no sensor found within %d dBm in %u s\n",
                                  K.name, s_prox, PAIR_TIMEOUT_UNITS * 5 / 2);
                    break;
                case EV_START_FAIL:
                    diag::log("ant node start failed: %s",
                              ant_espphy_status_str((ant_espphy_status_t)e.status));
                    break;
            }
        }
        // Housekeeping at 1 Hz: a channel the ANT stack still calls "tracking"
        // but that has gone quiet is dropped here, so the connected flag does
        // not outlive the data (a search never raises RX_FAIL on its own).
        uint32_t now = millis();
        for (int k = 0; k < ant_sensors::KIND_COUNT; ++k) {
            Kind& K = kinds[k];
            if (K.published && (!K.tracking || now - K.lastPageMs > STALE_MS)) {
                K.tracking = false;
                markConnected(k, false);
            }
            // A pair request the arbiter never got to serve (or whose timeout
            // event was lost with a node stop) must not hold the radio forever.
            if (K.pairing && now - K.pairStartMs > PAIR_TIMEOUT_MS) {
                K.pairing = false;
                Serial.printf("[ant] %s pair search abandoned\n", K.name);
            }
        }
    }
}

}  // namespace

extern "C" void* r_emi_get_mem_addr_by_offset(uint16_t offset);

namespace ant_sensors {

void dumpEm(unsigned off, unsigned len) {
    if (len > 4096) len = 4096;
    const uint8_t* em = (const uint8_t*)r_emi_get_mem_addr_by_offset((uint16_t)off);
    for (unsigned i = 0; i < len; i += 16) {
        Serial.printf("em %04x:", off + i);
        for (unsigned j = 0; j < 16 && i + j < len; ++j) Serial.printf(" %02x", em[i + j]);
        Serial.println();
    }
}

// Kept across node restarts so an experiment survives the arbiter's
// stop/start; copied into the phy after every start.
static uint8_t s_fmt = 0;
static uint8_t s_ch = 0;
static bool s_keepCrc = false;
static uint8_t s_patch = 0;
static uint8_t s_sync[4]; static bool s_syncOn = false;
static uint16_t s_mhz = 0;
static struct { uint8_t off, val; } s_ovr[4];
static uint8_t s_ovrN = 0;

static void applyKnobs() {
    if (!nodeUp) return;
    node.phy.cs_fmt_override = s_fmt;
    node.phy.coexist_ch = s_ch;
    node.phy.win_keep_crc = s_keepCrc;
    node.phy.patch_mask = s_patch;
    memcpy(node.phy.sync_override, s_sync, 4);
    node.phy.sync_override_on = s_syncOn;
    node.phy.mhz_override = s_mhz;
    node.phy.cs_ovr_n = 0;
    for (unsigned i = 0; i < s_ovrN; ++i) { node.phy.cs_ovr[i].off = s_ovr[i].off; node.phy.cs_ovr[i].val = s_ovr[i].val; }
    node.phy.cs_ovr_n = s_ovrN;
}

void setProximity(int8_t dbm) {
    s_prox = dbm;
    if (nodeUp) node.cfg.proximity_rssi = dbm;   // read at every channel open
    Serial.printf("[ant] pair proximity gate: %d dBm%s\n", dbm, dbm ? "" : " (any)");
}

void setCsFormat(uint8_t fmt) {
    s_fmt = fmt;
    applyKnobs();
    Serial.printf("[ant] scan CS format override: 0x%02x%s\n", fmt, fmt ? "" : " (keep)");
}

void setCoexistChannel(uint8_t ch) {
    s_ch = ch;
    applyKnobs();
    Serial.printf("[ant] coexist channel index: %u%s\n", ch, ch ? "" : " (default 39)");
}

void setSyncOverride(const char* hex8) {
    if (!hex8 || strlen(hex8) < 8) { s_syncOn = false; applyKnobs(); Serial.println("[ant] sync override off"); return; }
    for (int i = 0; i < 4; ++i) { char b[3] = {hex8[2*i], hex8[2*i+1], 0}; s_sync[i] = (uint8_t)strtoul(b, nullptr, 16); }
    s_syncOn = true; applyKnobs();
    Serial.printf("[ant] sync override %02x %02x %02x %02x (on-air order)\n", s_sync[0], s_sync[1], s_sync[2], s_sync[3]);
}
void setMhz(uint16_t mhz) { s_mhz = mhz; applyKnobs(); Serial.printf("[ant] window frequency override: %u MHz%s\n", mhz, mhz ? "" : " (off)"); }
void setPatchMask(uint8_t m) { s_patch = m; applyKnobs(); Serial.printf("[ant] window patch mask 0x%02x (1 sync, 2 freq, 4 rate, 8 rxmax; 0 = all)\n", m); }
void setKeepCrc(bool keep) { s_keepCrc = keep; applyKnobs(); Serial.printf("[ant] window CRC check: %s\n", keep ? "kept on" : "off"); }

void setCsOverride(uint8_t off, uint8_t val) {
    if (off == 0xFF) { s_ovrN = 0; applyKnobs(); Serial.println("[ant] CS overrides cleared"); return; }
    for (unsigned i = 0; i < s_ovrN; ++i) if (s_ovr[i].off == off) { s_ovr[i].val = val; applyKnobs(); return; }
    if (s_ovrN < 4) { s_ovr[s_ovrN].off = off; s_ovr[s_ovrN].val = val; s_ovrN++; }
    applyKnobs();
    Serial.printf("[ant] CS[%u] := 0x%02x at every reschedule (%u overrides)\n", off, val, s_ovrN);
}

struct BootTrace { uint32_t t; const char* type; uint8_t a; uint16_t b; };
static BootTrace s_bootTrace[48];
static int s_bootTraceN = 0;

void printBootTrace() {
    Serial.printf("[ant] %d radio events before the last reset (us delta, type, a, b):\n", s_bootTraceN);
    uint32_t prev = 0;
    for (int i = 0; i < s_bootTraceN; ++i) {
        const BootTrace& bt = s_bootTrace[i];
        Serial.printf("  %+8ld %-6s %3u %5u\n", prev ? (long)(bt.t - prev) : 0L, bt.type, bt.a, bt.b);
        prev = bt.t;
    }
}

void begin() {
    // The radio-event trace the library keeps in RTC memory survives a
    // watchdog reset: log the tail of the previous run so a crash on the
    // road can be read from the card (the panic text never reaches USB).
    {
        size_t n = ant_espphy_trace_count();
        // Keep a copy for the console (`ant trace`): the card is not always
        // reachable from the bench, the USB console is.
        s_bootTraceN = 0;
        for (size_t i = (n > 48 ? n - 48 : 0); i < n && s_bootTraceN < 48; ++i) {
            BootTrace& bt = s_bootTrace[s_bootTraceN];
            if (!ant_espphy_trace_get(i, &bt.t, &bt.type, &bt.a, &bt.b)) break;
            s_bootTraceN++;
        }
        if (n) {
            size_t from = n > 40 ? n - 40 : 0;
            char line[160];
            int o = 0;
            uint32_t prev = 0;
            for (size_t i = from; i < n; ++i) {
                uint32_t t; const char* ty; uint8_t a; uint16_t b;
                if (!ant_espphy_trace_get(i, &t, &ty, &a, &b)) break;
                o += snprintf(line + o, sizeof(line) - o, "%s%+ld/%u/%u ", ty,
                              prev ? (long)(t - prev) : 0L, a, b);
                prev = t;
                if (o > (int)sizeof(line) - 30) { diag::log("ant trace: %s", line); o = 0; }
            }
            if (o) diag::log("ant trace: %s", line);
            diag::log("ant trace: %u events from the previous run (end)", (unsigned)n);
        }
        ant_espphy_trace_reset();
    }
    ant_known_init(&known);
    if (ant_node_store_nvs.load(&ant_node_store_nvs, &known)) {
        for (unsigned i = 0; i < known.count; ++i)
            Serial.printf("[ant] remembered device %u type %u\n", known.dev[i].device_num,
                          known.dev[i].device_type);
    }
    candMutex = xSemaphoreCreateMutex();
    evq = xQueueCreate(32, sizeof(Ev));
    xTaskCreatePinnedToCore(feedTask, "antfeed", 3072, nullptr, 1, nullptr, 1);
}

static bool s_hold = false;
void setHold(bool on) { s_hold = on; }
bool hold() { return s_hold; }

bool wanted() {
    // BENCH GATE (2026-09-02): the coexist radio has been resetting the board
    // shortly after every boot, and with a paired sensor the arbiter starts
    // ANT within the first 90 s on its own, which turned that into a boot
    // loop. Until the cause is found, ANT runs only on an explicit request:
    // `ant hold on`, or a pair search. Remove this once the radio is stable.
    if (scanActive()) return true;
    if (!s_hold && !pairPending()) return false;
    for (int k = 0; k < KIND_COUNT; ++k)
        if (kinds[k].pairing || isPaired(k)) return true;
    return false;
}

bool pairPending() {
    for (int k = 0; k < KIND_COUNT; ++k)
        if (kinds[k].pairing) return true;
    return false;
}

bool radioActive() { return nodeUp; }

void setRadio(bool on) {
    if (on == nodeUp) return;
    if (on) {
        ant_node_config_t cfg{};
        cfg.on_data = onData;
        cfg.on_event = onEvent;
        cfg.on_paired = onPaired;
        cfg.store = &ant_node_store_nvs;
        cfg.proximity_rssi = s_prox;
        cfg.coexist = true;     // NimBLE keeps the controller; we ride its scan
        ant_espphy_status_t s = ant_node_start(&node, &cfg);
        if (s != ANT_ESPPHY_OK) {
            Serial.printf("[ant] start failed: %s\n", ant_espphy_status_str(s));
            Ev e{};
            e.type = EV_START_FAIL;
            e.status = (uint8_t)s;
            post(e);
            // Give up on any pair request rather than retrying every second.
            for (int k = 0; k < KIND_COUNT; ++k) kinds[k].pairing = false;
            return;
        }
        nodeUp = true;
        applyKnobs();
        openChannels();
    } else {
        // `tracking` is left alone: a stop for a 5 s BLE hunt followed by a
        // reopen on the remembered id (no search) resumes pages well inside
        // the 15 s staleness window, so the field never flickers. It also
        // keeps anyTracking() true, which is what makes the arbiter bring ANT
        // straight back after the hunt. A sensor that is really gone falls to
        // the feed task's staleness check like any other.
        ant_node_stop(&node);
        nodeUp = false;
    }
}

bool tracking(int kind) {
    return kind >= 0 && kind < KIND_COUNT && kinds[kind].tracking;
}

bool anyTracking() {
    for (int k = 0; k < KIND_COUNT; ++k)
        if (kinds[k].tracking) return true;
    return false;
}

void links(Link* out) {
    for (int k = 0; k < KIND_COUNT; ++k) {
        Link& l = out[k];
        memset(&l, 0, sizeof(l));
        Kind& K = kinds[k];
        snprintf(l.kind, sizeof(l.kind), "%s", K.name);
        const ant_known_device_t* d = knownFor(k);
        l.paired = d != nullptr;
        l.deviceNum = d ? d->device_num : 0;
        l.tracking = K.tracking;
        l.pairing = K.pairing;
        l.searching = nodeUp && !K.tracking &&
                      (ant_node_channel_status(&node, (uint8_t)k) & ANT_STATUS_STATE_MASK) ==
                          ANT_STATUS_SEARCHING;
        l.rssi = K.rssi;
        l.pages = K.pages;
        l.lastPageMs = K.lastPageMs;
    }
}

bool pair(int kind) {
    if (kind < 0 || kind >= KIND_COUNT) return false;
    Kind& K = kinds[kind];
    K.pairing = true;
    K.pairStartMs = millis();
    Serial.printf("[ant] %s: searching for a sensor within %d dBm (%u s)...\n", K.name,
                  s_prox, PAIR_TIMEOUT_UNITS * 5 / 2);
    // If the node is already up, search now; otherwise the arbiter brings it
    // up on its next tick and openChannels() sees the flag.
    if (nodeUp) openPairSearch(kind);
    return true;
}

bool forget(int kind) {
    if (kind < 0 || kind >= KIND_COUNT) return false;
    Kind& K = kinds[kind];
    K.pairing = false;
    bool had;
    if (nodeUp) {
        ant_node_close(&node, (uint8_t)kind);
        had = ant_node_forget_device(&node, K.devType);   // saves the store
        if (K.altType) had |= ant_node_forget_device(&node, K.altType);
        ant_known_remove(&known, K.devType);
        if (K.altType) ant_known_remove(&known, K.altType);
    } else {
        had = ant_known_remove(&known, K.devType);
        if (K.altType) had |= ant_known_remove(&known, K.altType);
        if (had) ant_node_store_nvs.save(&ant_node_store_nvs, &known);
    }
    K.tracking = false;
    if (had) Serial.printf("[ant] forgot %s\n", K.name);
    return had;
}

void forgetAll() {
    for (int k = 0; k < KIND_COUNT; ++k) forget(k);
}

void displayName(int kind, char* out, size_t n) {
    if (kind < 0 || kind >= KIND_COUNT) { if (n) out[0] = 0; return; }
    const ant_known_device_t* d = knownFor(kind);
    if (!d) { if (n) out[0] = 0; return; }
    snprintf(out, n, "ANT+ %s %u", kinds[kind].name, d->device_num);
}

void types(bool start) {
    if (start) {
        memset(tally, 0, sizeof(tally)); tallyTotal = 0;
        tapRemaining = -1;
        if (nodeUp) node.phy.tap_on = true;
        Serial.println("[ant] tallying decoded frames by device type");
        return;
    }
    Serial.printf("[ant] %lu frames since tally start:\n", (unsigned long)tallyTotal);
    for (int t = 0; t < 256; ++t) {
        if (!tally[t].n) continue;
        Serial.printf("  type 0x%02x (%3d): %5lu frames, %5lu matched, best rssi %d dBm\n", t, t,
                      (unsigned long)tally[t].n, (unsigned long)tally[t].matched, tally[t].bestRssi);
    }
}

void tap(int n) {
    if (!nodeUp) { Serial.println("[ant] radio is down; pair or wait for it to come up"); return; }
    tapRemaining = n;
    node.phy.tap_on = true;
    Serial.printf("[ant] tapping the next %d frames (body = mk mk dn dn type trans | flags page[8] | crc)\n", n);
}

static void applyScan(bool before) {
    bool now = scanActive();
    if (now == before || !nodeUp) return;
    if (now) openScan();
    else {
        ant_node_close(&node, SCAN_CH);
        if (tapRemaining == 0) node.phy.tap_on = false;
    }
}
void scanSet(bool on) { bool b = scanActive(); s_scan = on; applyScan(b); }
void scanWhileListOpen(bool on) { bool b = scanActive(); s_listScan = on; applyScan(b); }
bool scanning() { return scanActive(); }

int candidates(Candidate* out, int maxOut) {
    if (!candMutex) return 0;
    xSemaphoreTake(candMutex, portMAX_DELAY);
    int n = candN < maxOut ? candN : maxOut;
    memcpy(out, cands, n * sizeof(Candidate));
    xSemaphoreGive(candMutex);
    return n;
}

int kindForType(uint8_t t) {
    for (int k = 0; k < KIND_COUNT; ++k)
        if (kinds[k].devType == t || (kinds[k].altType && kinds[k].altType == t)) return k;
    return -1;
}

const char* typeName(uint8_t t) {
    int k = kindForType(t);
    if (k >= 0) return kinds[k].name;
    static char buf[12];
    snprintf(buf, sizeof(buf), "type %u", t);
    return buf;
}

bool pairDevice(uint8_t devType, uint16_t devNum, uint8_t trans) {
    int k = kindForType(devType);
    if (k < 0 || !devNum) return false;
    // One device per kind: drop whatever served it before (either type).
    ant_known_remove(&known, kinds[k].devType);
    if (kinds[k].altType) ant_known_remove(&known, kinds[k].altType);
    ant_known_device_t dev{devNum, devType, trans};
    ant_known_put(&known, &dev);
    if (nodeUp) {
        ant_node_forget_device(&node, kinds[k].devType);
        if (kinds[k].altType) ant_node_forget_device(&node, kinds[k].altType);
        ant_node_remember_device(&node, &dev);       // saves the store
        kinds[k].pairing = false;
        ant_node_close(&node, (uint8_t)k);
        openPaired(k);
    } else {
        ant_node_store_nvs.save(&ant_node_store_nvs, &known);
    }
    kinds[k].tracking = false;
    diag::log("ant %s paired by id: device %u type %u", kinds[k].name, devNum, devType);
    Serial.printf("[ant] %s paired #%u (type %u)\n", kinds[k].name, devNum, devType);
    return true;
}

void printReport() {
    Serial.printf("[ant] radio %s (coexist: rides NimBLE's passive scan; BLE scanning "
                  "pauses while up)\n", nodeUp ? "UP" : "down");
    Link ls[KIND_COUNT];
    links(ls);
    for (int k = 0; k < KIND_COUNT; ++k) {
        const Link& l = ls[k];
        const char* st = l.tracking ? "TRACKING" : l.pairing ? "pairing..."
                       : l.searching ? "searching" : nodeUp ? "closed" : "idle";
        if (!l.paired && !l.pairing) {
            Serial.printf("  %-8s (not paired)\n", l.kind);
            continue;
        }
        Serial.printf("  %-8s #%-5u %-10s rssi %4d dBm  %lu pages", l.kind, l.deviceNum,
                      st, l.rssi, (unsigned long)l.pages);
        if (l.lastPageMs) Serial.printf("  last %.1f s ago", (millis() - l.lastPageMs) / 1000.0f);
        Serial.println();
    }
    if (nodeUp) {
        const ant_espphy_t* r = ant_node_radio(&node);
        Serial.printf("  phy: sched=%lu rxhook=%lu frames=%lu matched=%lu dropped=%lu "
                      "mode=%s cs=0x%x\n", (unsigned long)r->sched_hook_calls,
                      (unsigned long)r->rx_hook_calls, (unsigned long)r->rx_frames,
                      (unsigned long)r->rx_matched, (unsigned long)r->rx_dropped + (unsigned long)r->rx_off_dropped,
                      ant_espphy_mode_str(r->mode), (unsigned)r->cs_off);
        Serial.printf("  phy: windows=%lu crc %s patch=0x%02x\n", (unsigned long)r->win_hook_calls,
                      r->win_keep_crc ? "kept ON (knob)" : "off per window", r->patch_mask);
        Serial.printf("  phy: scan rx isr=%lu last cntl|stat=%08lx hdr=%08lx\n",
                      (unsigned long)r->scan_rx_isr_calls, (unsigned long)r->scan_rx_isr_stat,
                      (unsigned long)r->scan_rx_isr_hdr);
        Serial.printf("  phy: last rx desc cntl|stat=%08lx hdr=%08lx (type %02lx len %lu rssi %d)\n",
                      (unsigned long)r->rx_last_stat, (unsigned long)r->rx_last_hdr,
                      (unsigned long)(r->rx_last_hdr & 0xff), (unsigned long)((r->rx_last_hdr >> 8) & 0xff),
                      (int)(int8_t)((r->rx_last_hdr >> 16) & 0xff));
    }
    if (scanActive()) {
        Candidate cs[MAX_CANDS];
        int n = candidates(cs, MAX_CANDS);
        Serial.printf("  scan: %d device(s) heard\n", n);
        for (int i = 0; i < n; ++i)
            Serial.printf("    %-8s #%-5u type %3u trans %3u  %4d dBm  %lu frames, %.0f s ago\n",
                          typeName(cs[i].devType), cs[i].devNum, cs[i].devType, cs[i].trans,
                          cs[i].rssi, (unsigned long)cs[i].frames, (millis() - cs[i].lastMs) / 1000.0);
    }
    Serial.println("  'ant pair <hr|power|cadence>' searches nearby; 'ant scan on|off' lists devices; 'ant forget <kind|all>'");
}

}  // namespace ant_sensors
