#include "lora_radio.h"

#include <Arduino.h>
#include <RadioLib.h>
#include <SPI.h>
#include <cmath>

#include "config.h"
#include "diag.h"
#include "sd_bus.h"

namespace {

// RadioLib drives CS/RST itself; IRQ is DIO1 and BUSY is the SX1262's handshake
// line. The SPI instance is the global one the SD card already opened in
// ride_recorder::begin() — deliberately shared, since it is one physical bus.
SX1262 radio = new Module(BOARD_LORA_CS, BOARD_LORA_IRQ, BOARD_LORA_RST,
                          BOARD_LORA_BUSY);

bool radioOk = false;
int lastErr = RADIOLIB_ERR_NONE;
lora_radio::Config cfg;

// One DIO1 interrupt serves both directions — the radio is never receiving and
// transmitting at once, so `txInFlight` says which event the flag means.
volatile bool dio1 = false;
bool txInFlight = false;

void IRAM_ATTR onDio1() { dio1 = true; }

// Symbol duration in microseconds at the configured spreading factor.
float symbolUs() {
    return (float)(1u << cfg.sf) / (cfg.bwKhz / 1000.0f);
}

}  // namespace

namespace lora_radio {

bool begin(const Config& c) {
    cfg = c;

    // The rail is raised in setup() (board_radio_power) — the module and the GPS
    // share it, so it is already on by the time anything gets here.
    //
    // CS high before anything else: SD and LoRa share MISO/MOSI/SCK, and a
    // floating LoRa CS corrupts SD traffic. ride_recorder::begin() does the same
    // for its own card, and SPI.begin() has already run there. Calling it twice
    // is harmless, and this covers a build where the SD never mounted.
    pinMode(BOARD_LORA_CS, OUTPUT);
    digitalWrite(BOARD_LORA_CS, HIGH);
    SPI.begin(BOARD_SPI_SCLK, BOARD_SPI_MISO, BOARD_SPI_MOSI);

    sdLock();
    int st = radio.begin(cfg.freqMHz, cfg.bwKhz, cfg.sf, cfg.cr, cfg.syncWord,
                         cfg.powerDbm, cfg.preambleLen, BOARD_LORA_TCXO_V,
                         /*useRegulatorLDO=*/false);
    if (st == RADIOLIB_ERR_NONE) {
        // DIO2 gates the RF switch and DIO3 feeds the TCXO on this module; both
        // are LilyGO board facts, not choices (see their lora_send example).
        radio.setDio2AsRfSwitch(true);
        // PA current cap. 140 mA is what LilyGO's own examples use at 22 dBm.
        radio.setCurrentLimit(140);
        // Meshtastic runs with the CRC on, and a receiver configured otherwise
        // reads every packet as corrupt.
        radio.setCRC(RADIOLIB_SX126X_LORA_CRC_ON);
        // ~2 dB of extra sensitivity for ~2 mA. On a bike computer that is the
        // right side of the trade: the marginal packet is the one worth having.
        radio.setRxBoostedGainMode(true);
        radio.setDio1Action(onDio1);
    }
    sdUnlock();

    lastErr = st;
    radioOk = (st == RADIOLIB_ERR_NONE);
    if (!radioOk) {
        diag::log("lora: SX1262 init FAILED (RadioLib %d)", st);
        return false;
    }
    diag::log("lora: SX1262 up on %.4f MHz, SF%u BW%.0f CR4/%u, %d dBm",
              cfg.freqMHz, cfg.sf, cfg.bwKhz, cfg.cr, cfg.powerDbm);
    txInFlight = false;
    listen();
    return true;
}

bool ok() { return radioOk; }
float frequencyMHz() { return cfg.freqMHz; }
int lastError() { return lastErr; }
bool irqFired() { return dio1; }
bool transmitting() { return txInFlight; }

void listen() {
    if (!radioOk) return;
    dio1 = false;
    txInFlight = false;
    sdLock();
    lastErr = radio.startReceive();
    sdUnlock();
}

size_t read(uint8_t* buf, size_t cap, float& rssiDbm, float& snrDb) {
    if (!radioOk) return 0;
    dio1 = false;
    sdLock();
    const size_t len = radio.getPacketLength();
    size_t got = 0;
    if (len > 0 && len <= cap) {
        const int st = radio.readData(buf, len);
        lastErr = st;
        // CRC_MISMATCH means the frame arrived damaged. Reported, not returned:
        // a corrupt packet is normal at the edge of range, and a caller that saw
        // bytes here would try to decrypt noise.
        if (st == RADIOLIB_ERR_NONE) {
            got = len;
            rssiDbm = radio.getRSSI();
            snrDb = radio.getSNR();
        }
    } else if (len > cap) {
        // Longer than any legal Meshtastic frame — drain it so the FIFO does not
        // stall, and drop it.
        radio.readData(buf, cap);
        lastErr = RADIOLIB_ERR_PACKET_TOO_LONG;
    }
    // Straight back to listening inside the same lock: the gap between reading a
    // packet and re-arming is time the mesh is inaudible.
    radio.startReceive();
    sdUnlock();
    return got;
}

bool channelBusy() {
    if (!radioOk) return false;
    sdLock();
    const int st = radio.scanChannel();
    // Back to listening before returning, whatever the answer.
    //
    // LOAD-BEARING. A channel-activity scan leaves the SX1262 in standby, so
    // without this the radio goes deaf the moment the outbox has anything in it:
    // the caller backs off on a busy channel and returns, and nothing re-arms
    // receive until the next successful transmission. startTransmit() switches
    // out of receive by itself, so re-arming here costs the sender nothing.
    radio.startReceive();
    sdUnlock();
    // Anything other than a clean "nothing there" is treated as busy, including
    // an error: backing off costs one slot, transmitting over someone else costs
    // both packets.
    const bool free = (st == RADIOLIB_CHANNEL_FREE);
    if (!free && st != RADIOLIB_LORA_DETECTED) lastErr = st;
    dio1 = false;   // CAD raises DIO1 too; that is not our packet event
    return !free;
}

void sleep() {
    if (!radioOk) return;
    dio1 = false;
    txInFlight = false;
    sdLock();
    // retainConfig, so waking is a startReceive() rather than a full re-init.
    radio.sleep(true);
    sdUnlock();
    // NOTE: the 3V3 rail is deliberately left alone. board_radio_power() gates
    // the module AND the GPS from one expander pin, so cutting it here would
    // silently kill navigation.
}

bool startSend(const uint8_t* buf, size_t len) {
    if (!radioOk || txInFlight) return false;
    dio1 = false;
    sdLock();
    const int st = radio.startTransmit((uint8_t*)buf, len);
    sdUnlock();
    lastErr = st;
    if (st != RADIOLIB_ERR_NONE) {
        diag::log("lora: startTransmit failed (%d)", st);
        listen();
        return false;
    }
    txInFlight = true;
    return true;
}

void finishSend() {
    if (!radioOk) return;
    sdLock();
    radio.finishTransmit();
    sdUnlock();
    listen();
}

void abortSend() {
    if (!radioOk) return;
    diag::log("lora: transmit timed out — resetting to receive");
    sdLock();
    radio.standby();
    radio.finishTransmit();
    sdUnlock();
    listen();
}

float instantRssi() {
    if (!radioOk) return 0.0f;
    sdLock();
    // packet=false is the live RssiInst register rather than the last packet's
    // signal, which is what makes this a noise-floor reading.
    const float v = radio.getRSSI(false);
    sdUnlock();
    return v;
}

uint32_t airtimeMs(size_t len) {
    // Semtech's LoRa time-on-air, explicit header, low-data-rate optimisation on
    // for SF11/SF12 (which is what the SX1262 does automatically at these
    // bandwidths, and what Meshtastic's own airtime accounting assumes).
    const float tSym = symbolUs() / 1000.0f;                 // ms
    const int de = (cfg.sf >= 11) ? 1 : 0;
    const float num = (float)(8 * (int)len) - 4.0f * cfg.sf + 28.0f + 16.0f;
    const float den = 4.0f * (float)(cfg.sf - 2 * de);
    float payloadSym = std::ceil(num / den) * (float)cfg.cr;
    if (payloadSym < 0) payloadSym = 0;
    const float total = (cfg.preambleLen + 4.25f + 8.0f + payloadSym) * tSym;
    return (uint32_t)(total + 0.5f);
}

}  // namespace lora_radio
