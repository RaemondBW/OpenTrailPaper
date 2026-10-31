// The SX1262 LoRa radio (src/lora_radio.h) and the air around it.
//
// Fitted only with `--lora`; without it the module answers nothing, as the
// board's "not fitted" path (and every other test) expects. When fitted, it
// keeps lora_radio's contract as the RadioLib-driven chip does:
//
//  * begin() tunes it (frequency, bandwidth, spreading factor, coding rate, sync
//    word) and leaves it listening; sleep() deafens it until listen().
//  * A transmission lasts its LoRa time-on-air (Semtech's formula, the one
//    lora_radio::airtimeMs uses) of device time; DIO1 fires when it is done.
//    The radio is half-duplex: what arrives while it transmits is lost, and
//    at TxDone it drops to standby, receiving again only once the firmware
//    calls finishSend()/listen().
//  * A received frame is heard only if the radio is listening, tuned to the
//    sender's frequency, bandwidth, spreading factor and sync word (the coding
//    rate travels in the LoRa header, so it need not match), and it arrives a
//    time-on-air after it was sent. Frames share one channel: each starts only
//    when the one before it has finished, so they never overlap (collisions are
//    not modelled).
//  * The FIFO holds ONE packet. Continuous receive writes every packet at the
//    same buffer address, so a packet not read before the next one has arrived
//    is overwritten — counted as `overrun`. This is the chip's real behaviour,
//    and what makes the mesh task's poll interval matter.
//  * A frame can arrive damaged (CRC error): DIO1 fires, read() returns 0.
//  * Channel-activity detection reports busy only when a test says so.
//
// The air is the control channel: `lora_rx` puts a frame on it for this radio
// (a virtual node, or another sim's transmission relayed by otpsim.air),
// `lora_tx` reads back what this radio transmitted, and `lora` sets the link's
// impairments (loss, latency, RSSI/SNR, CRC errors, a busy channel) and reports
// counters. See sim/PROTOCOL.md.
#include <Arduino.h>

#include "diag.h"
#include "json.h"
#include "lora_radio.h"
#include "sd_bus.h"
#include "sim.h"
#include "sim_clock.h"

#include <cmath>
#include <deque>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

namespace {

struct Tune {
    float freqMHz = 0, bwKhz = 0;
    int sf = 0, cr = 5, sync = 0x2B, preamble = 16;
    bool same(const Tune& o) const {
        return std::fabs(freqMHz - o.freqMHz) < 0.001f && std::fabs(bwKhz - o.bwKhz) < 0.01f && sf == o.sf &&
               sync == o.sync;
    }
};

struct RxFrame {
    std::vector<uint8_t> data;
    Tune tune;
    float rssi = -70, snr = 8;
    bool crcBad = false;
    uint32_t startMs = 0, endMs = 0;   // device time it is on the air
};

struct TxRecord {
    uint64_t seq;
    uint32_t startMs, endMs;
    Tune tune;
    int powerDbm;
    std::vector<uint8_t> data;
    bool aborted = false;
};

enum Mode { OFF, SLEEP, STANDBY, RX, TX };

std::recursive_mutex g_m;
lora_radio::Config g_cfg;
Tune g_tune;
Mode g_mode = OFF;
bool g_ok = false;
std::deque<RxFrame> g_air;           // frames on their way to us, by end time
uint32_t g_airFreeAt = 0;            // when the channel is next free (device ms)
// When the radio was NOT listening (sleep, standby, transmitting, retuning):
// anything that ends inside one of these spans was not heard.
struct Span { uint32_t from, to; };
std::deque<Span> g_deaf;
Span g_lastDeaf{0, 0};               // the last frame lost to it, for diagnosis
uint32_t g_deafSince = 0;
bool g_listening = false;
std::vector<uint8_t> g_txFrame;
uint32_t g_txStart = 0, g_txEnd = 0;
std::deque<TxRecord> g_txLog;
uint64_t g_txSeq = 0;
// The FIFO: the one received frame waiting for read().
bool g_fifoFull = false;
RxFrame g_fifo;
// Frames the firmware would already have read on the board. See irqModel().
std::deque<RxFrame> g_held;

// Impairments, set by `lora`.
double g_lossPct = 0, g_crcPct = 0;
uint32_t g_latencyMs = 0;
float g_rssi = -70, g_snr = 8;
bool g_busy = false;
bool g_modelOverrun = true;
std::mt19937 g_rng(1);

// Counters.
struct Counters {
    uint32_t queued = 0, heard = 0, lost = 0, mistuned = 0, deaf = 0, overrun = 0, crc = 0, read = 0, tx = 0,
             cadBusy = 0, begins = 0, irqs = 0, turnMaxMs = 0, turnLastMs = 0;
} g_n;

uint32_t now() { return sim_clock::millis(); }

// DIO1. The chip raises it the moment a frame has finished going out or
// coming in; here a thread sleeps on the device clock until then and calls
// the hook, which (as on the board) does nothing but wake the mesh task. The
// task then asks irqFired() what happened, exactly as it does on hardware.
void (*g_irqHook)() = nullptr;
bool irqModel() { return g_irqHook != nullptr; }
void fireIrqAt(uint32_t atMs) {
    if (!g_irqHook) return;
    std::thread([atMs] {
        const uint32_t t = now();
        if ((int32_t)(atMs - t) > 0) sim_clock::sleepMs(atMs - t);
        g_n.irqs++;
        if (g_irqHook) g_irqHook();
    }).detach();
}

uint32_t airtime(const Tune& t, size_t len) {
    if (t.sf <= 0 || t.bwKhz <= 0) return (uint32_t)len * 8;
    const float tSym = (float)(1u << t.sf) / (t.bwKhz / 1000.0f) / 1000.0f;   // ms
    const int de = (t.sf >= 11) ? 1 : 0;
    const float num = (float)(8 * (int)len) - 4.0f * t.sf + 28.0f + 16.0f;
    const float den = 4.0f * (float)(t.sf - 2 * de);
    float payloadSym = std::ceil(num / den) * (float)t.cr;
    if (payloadSym < 0) payloadSym = 0;
    return (uint32_t)((t.preamble + 4.25f + 8.0f + payloadSym) * tSym + 0.5f);
}

// What the firmware's interrupt buys it, modelled in device time. On the
// board a firmware that wakes on DIO1 (it registered onIrq) re-arms the
// receiver and reads a frame within microseconds of the interrupt. Here the
// clock is real time, so the same wake costs a host thread hop — and on a
// loaded CI runner that was measured at 130-410 ms, which a test of a 10 ms
// turnaround then blames on the firmware. So with a hook registered, the
// receiver counts as listening again from the instant our frame ended, and a
// frame that lands before the last was read waits rather than overwriting
// it. A firmware without the hook (the 250 ms poll this replaced) is modelled
// exactly as before: deaf until it calls listen(), one frame of buffer.

void setListening(bool on, uint32_t heardFrom = 0) {
    if (on == g_listening) return;
    g_listening = on;
    const uint32_t t = now();
    if (on) {
        const uint32_t end = heardFrom && (int32_t)(t - heardFrom) > 0 &&
                             (int32_t)(heardFrom - g_deafSince) >= 0 ? heardFrom : t;
        g_deaf.push_back({g_deafSince, end});
        while (g_deaf.size() > 64) g_deaf.pop_front();
    } else {
        g_deafSince = t;
    }
}

bool wasDeaf(uint32_t from, uint32_t to) {
    if (!g_listening && to >= g_deafSince) return true;
    for (auto& s : g_deaf)
        if (to >= s.from && from <= s.to) return true;
    return false;
}

// Moves every frame whose end has come into the FIFO (or the bin). Caller
// holds g_m.
void settle() {
    const uint32_t t = now();
    while (!g_air.empty() && (int32_t)(t - g_air.front().endMs) >= 0) {
        RxFrame f = std::move(g_air.front());
        g_air.pop_front();
        if (!g_ok || !f.tune.same(g_tune)) { g_n.mistuned++; continue; }
        // A packet is only received if the radio listened to all of it: the
        // preamble starts the demodulator.
        if (wasDeaf(f.startMs, f.endMs)) {
            g_n.deaf++;
            g_lastDeaf = {f.startMs, f.endMs};
            continue;
        }
        if (g_fifoFull && irqModel()) g_held.push_back(std::move(g_fifo));
        else if (g_fifoFull && g_modelOverrun) g_n.overrun++;
        g_fifo = std::move(f);
        g_fifoFull = true;
    }
}

std::string hex(const std::vector<uint8_t>& v) {
    static const char* d = "0123456789abcdef";
    std::string s;
    s.reserve(v.size() * 2);
    for (uint8_t b : v) { s += d[b >> 4]; s += d[b & 15]; }
    return s;
}

bool unhex(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    out.clear();
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back((uint8_t)(a << 4 | b));
    }
    return true;
}

std::string tuneJson(const Tune& t) {
    std::ostringstream o;
    o.precision(10);
    o << "\"freq_mhz\":" << t.freqMHz << ",\"bw_khz\":" << t.bwKhz << ",\"sf\":" << t.sf << ",\"cr\":" << t.cr
      << ",\"sync_word\":" << t.sync;
    return o.str();
}

}  // namespace

extern "C" void sim_efuse_mac(uint8_t* mac) { memcpy(mac, sim::opts.mac, 6); }

// ---- lora_radio, as the SX1262 behaves ---------------------------------------------------

namespace lora_radio {

bool begin(const Config& c) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    g_cfg = c;
    if (!sim::opts.lora) {
        g_ok = false;
        return false;
    }
    sdLock();   // the board's radio shares the card's SPI bus
    setListening(false);
    g_tune.freqMHz = c.freqMHz;
    g_tune.bwKhz = c.bwKhz;
    g_tune.sf = c.sf;
    g_tune.cr = c.cr;
    g_tune.sync = c.syncWord;
    g_tune.preamble = c.preambleLen;
    g_ok = true;
    g_mode = STANDBY;
    g_fifoFull = false;
    g_held.clear();
    g_n.begins++;
    sdUnlock();
    diag::log("lora: SX1262 up on %.4f MHz, SF%u BW%.0f CR4/%u, %d dBm", c.freqMHz, c.sf, c.bwKhz, c.cr,
              c.powerDbm);
    listen();
    return true;
}

bool ok() { return g_ok; }
float frequencyMHz() { return g_cfg.freqMHz; }
int lastError() { return g_ok ? 0 : -2; }
void onIrq(void (*fn)()) { g_irqHook = fn; }

bool irqFired() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return false;
    if (g_mode == TX) return (int32_t)(now() - g_txEnd) >= 0;
    if (g_mode != RX) return false;
    settle();
    return g_fifoFull || !g_held.empty();
}

bool transmitting() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    return g_ok && g_mode == TX;
}

size_t read(uint8_t* buf, size_t cap, float& rssiDbm, float& snrDb) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return 0;
    sdLock();
    settle();
    size_t got = 0;
    if (g_fifoFull || !g_held.empty()) {
        // Oldest first: a held frame came before the one in the FIFO.
        RxFrame f;
        if (!g_held.empty()) { f = std::move(g_held.front()); g_held.pop_front(); }
        else { f = std::move(g_fifo); g_fifoFull = false; }
        if (f.crcBad) {
            g_n.crc++;
        } else if (f.data.size() <= cap) {
            memcpy(buf, f.data.data(), f.data.size());
            got = f.data.size();
            rssiDbm = f.rssi;
            snrDb = f.snr;
            g_n.read++;
        }
    }
    g_mode = RX;
    setListening(true);
    sdUnlock();
    return got;
}

bool startSend(const uint8_t* buf, size_t len) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok || g_mode == TX) return false;
    sdLock();
    setListening(false);
    g_mode = TX;
    g_txFrame.assign(buf, buf + len);
    // A transmission starts when the channel is ours; anything still arriving
    // from the air is cut off (we stop listening now).
    g_txStart = now();
    g_txEnd = g_txStart + airtime(g_tune, len);
    fireIrqAt(g_txEnd);
    // Logged as it starts, so the air can relay it while it is still going
    // out and other radios' channel-activity detection sees it.
    g_txLog.push_back(TxRecord{++g_txSeq, g_txStart, g_txEnd, g_tune, g_cfg.powerDbm, g_txFrame});
    while (g_txLog.size() > 2000) g_txLog.pop_front();
    sdUnlock();
    return true;
}

void finishSend() {
    {
        std::lock_guard<std::recursive_mutex> lk(g_m);
        if (!g_ok) return;
        if (g_mode == TX) g_n.tx++;
    }
    listen();
}

void abortSend() {
    diag::log("lora: transmit timed out — resetting to receive");
    {
        std::lock_guard<std::recursive_mutex> lk(g_m);
        if (!g_txLog.empty() && g_txLog.back().startMs == g_txStart) g_txLog.back().aborted = true;
    }
    listen();
}

void listen() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return;
    // Turnaround: how long after our own frame ended the receiver came back.
    if (g_mode == TX || (g_mode == STANDBY && g_txEnd && (int32_t)(now() - g_txEnd) >= 0 &&
                         (int32_t)(now() - g_txEnd) < 5000)) {
        g_n.turnLastMs = now() - g_txEnd;
        if (g_n.turnLastMs > g_n.turnMaxMs) g_n.turnMaxMs = g_n.turnLastMs;
    }
    sdLock();
    const bool fromTx = g_mode == TX;
    g_mode = RX;
    setListening(true, fromTx && irqModel() ? g_txEnd : 0);
    sdUnlock();
}

bool channelBusy() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return false;
    sdLock();
    settle();
    // A frame in flight right now is activity too.
    bool busy = g_busy;
    const uint32_t t = now();
    for (auto& f : g_air)
        if (f.tune.same(g_tune) && (int32_t)(t - f.startMs) >= 0) busy = true;
    if (busy) g_n.cadBusy++;
    g_mode = RX;
    setListening(true);
    sdUnlock();
    return busy;
}

void sleep() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return;
    sdLock();
    g_mode = SLEEP;
    setListening(false);
    g_fifoFull = false;
    g_held.clear();
    sdUnlock();
}

float instantRssi() {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    if (!g_ok) return 0.0f;
    return -112.0f + (float)(g_rng() % 60) / 10.0f;
}

uint32_t airtimeMs(size_t len) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    Tune t = g_tune;
    if (!t.sf) {
        t.sf = g_cfg.sf; t.bwKhz = g_cfg.bwKhz; t.cr = g_cfg.cr; t.preamble = g_cfg.preambleLen;
    }
    return airtime(t, len);
}

}  // namespace lora_radio

// ---- the control channel: lora, lora_rx, lora_tx ------------------------------------------

std::string loraCommand(const json::Value& r) {
    std::lock_guard<std::recursive_mutex> lk(g_m);
    const std::string cmd = r.str("cmd");
    std::ostringstream o;
    o.precision(10);
    if (cmd == "lora_rx") {
        if (!sim::opts.lora) return "\"ok\":false,\"error\":\"no LoRa radio fitted (start with --lora)\"";
        RxFrame f;
        if (!unhex(r.str("hex"), f.data) || f.data.empty() || f.data.size() > 255)
            return "\"ok\":false,\"error\":\"hex: 1-255 bytes\"";
        // Default: sent on the channel this radio is tuned to.
        f.tune = g_tune;
        if (r.has("freq_mhz")) f.tune.freqMHz = (float)r.num("freq_mhz");
        if (r.has("bw_khz")) f.tune.bwKhz = (float)r.num("bw_khz");
        if (r.has("sf")) f.tune.sf = (int)r.num("sf");
        if (r.has("cr")) f.tune.cr = (int)r.num("cr");
        if (r.has("sync_word")) f.tune.sync = (int)r.num("sync_word");
        f.rssi = (float)r.num("rssi", g_rssi);
        f.snr = (float)r.num("snr", g_snr);
        f.crcBad = r.boolean("crc_error", false) ||
                   (g_crcPct > 0 && (double)(g_rng() % 10000) < g_crcPct * 100.0);
        g_n.queued++;
        if (g_lossPct > 0 && (double)(g_rng() % 10000) < g_lossPct * 100.0) {
            g_n.lost++;
            return "\"ok\":true,\"lost\":true";
        }
        // On the air after the link's latency, and not before the channel is
        // free: frames follow each other, they do not overlap.
        uint32_t start = now() + g_latencyMs + (uint32_t)r.num("delay_ms", 0);
        // `elapsed_ms`: the sender started this long ago (the air relays a
        // transmission while it is going out), so it ends that much sooner.
        const uint32_t elapsed = (uint32_t)r.num("elapsed_ms", 0);
        if ((int32_t)(g_airFreeAt - start) > 0) start = g_airFreeAt;
        const uint32_t on = airtime(f.tune, f.data.size());
        f.startMs = start - (elapsed < on ? elapsed : on);
        f.endMs = f.startMs + on;
        g_airFreeAt = f.endMs;
        o << "\"ok\":true,\"lost\":false,\"start_ms\":" << f.startMs << ",\"end_ms\":" << f.endMs;
        fireIrqAt(f.endMs);
        g_air.push_back(std::move(f));
        return o.str();
    }
    if (cmd == "lora_tx") {
        const uint64_t since = (uint64_t)r.num("since", 0);
        o << "\"ok\":true,\"frames\":[";
        bool first = true;
        uint64_t last = since;
        for (auto& t : g_txLog) {
            if (t.seq <= since) continue;
            o << (first ? "" : ",") << "{\"seq\":" << t.seq << ",\"start_ms\":" << t.startMs << ",\"end_ms\":" << t.endMs
              << "," << tuneJson(t.tune) << ",\"power_dbm\":" << t.powerDbm << ",\"aborted\":" << (t.aborted ? "true" : "false") << ",\"hex\":\"" << hex(t.data) << "\"}";
            first = false;
            last = t.seq;
        }
        o << "],\"seq\":" << last << ",\"now_ms\":" << now();
        return o.str();
    }
    // `lora`: set impairments (any of them), and report.
    if (r.has("loss_pct")) g_lossPct = r.num("loss_pct");
    if (r.has("crc_error_pct")) g_crcPct = r.num("crc_error_pct");
    if (r.has("latency_ms")) g_latencyMs = (uint32_t)r.num("latency_ms");
    if (r.has("rssi")) g_rssi = (float)r.num("rssi");
    if (r.has("snr")) g_snr = (float)r.num("snr");
    if (r.has("busy")) g_busy = r.boolean("busy");
    if (r.has("overrun")) g_modelOverrun = r.boolean("overrun");
    if (r.has("seed")) g_rng.seed((uint32_t)r.num("seed"));
    if (r.has("airtime_len")) {
        o << "\"ok\":true,\"airtime_ms\":" << airtime(g_tune, (size_t)r.num("airtime_len"));
        return o.str();
    }
    settle();
    const char* modes[] = {"off", "sleep", "standby", "rx", "tx"};
    o << "\"ok\":true,\"lora\":{\"fitted\":" << (sim::opts.lora ? "true" : "false")
      << ",\"up\":" << (g_ok ? "true" : "false") << ",\"mode\":\"" << modes[g_mode] << "\"," << tuneJson(g_tune)
      << ",\"power_dbm\":" << (int)g_cfg.powerDbm << ",\"loss_pct\":" << g_lossPct << ",\"crc_error_pct\":" << g_crcPct
      << ",\"latency_ms\":" << g_latencyMs << ",\"rssi\":" << g_rssi << ",\"snr\":" << g_snr
      << ",\"busy\":" << (g_busy ? "true" : "false") << ",\"in_flight\":" << g_air.size()
      << ",\"fifo\":" << (g_fifoFull ? "true" : "false") << ",\"queued\":" << g_n.queued << ",\"lost\":" << g_n.lost
      << ",\"mistuned\":" << g_n.mistuned << ",\"deaf\":" << g_n.deaf << ",\"overrun\":" << g_n.overrun
      << ",\"crc\":" << g_n.crc << ",\"read\":" << g_n.read << ",\"tx\":" << g_n.tx << ",\"cad_busy\":" << g_n.cadBusy << ",\"irqs\":" << g_n.irqs << ",\"turn_max_ms\":" << g_n.turnMaxMs << ",\"turn_last_ms\":" << g_n.turnLastMs
      << ",\"last_deaf_frame\":[" << g_lastDeaf.from << "," << g_lastDeaf.to << "],\"deaf_spans\":[";
    for (size_t i = 0; i < g_deaf.size(); ++i) o << (i ? "," : "") << "[" << g_deaf[i].from << "," << g_deaf[i].to << "]";
    o << "],\"now_ms\":" << now() << ",\"listening\":" << (g_listening ? "true" : "false")
      << ",\"begins\":" << g_n.begins << ",\"tx_seq\":" << g_txSeq << "}";
    return o.str();
}
