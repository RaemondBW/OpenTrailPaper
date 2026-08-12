#pragma once

// SX1262 LoRa radio (the 915 MHz module on the T5 e-paper S3 PRO).
//
// A thin, non-blocking wrapper over RadioLib, shaped by one hard constraint: the
// radio sits on the SAME SPI bus as the SD card, so every transfer here has to
// take the sd_bus.h lock — and a LongFast packet is up to ~2.5 s in the air, far
// too long to hold a lock the 1 Hz ride recorder also needs. So transmit is
// split: stage the packet and start it under the lock, then release and watch
// the DIO1 interrupt for completion. Nothing in this file blocks on the air.
//
// One task (mesh_service) owns the radio. There is no internal locking against a
// second caller.

#include <cstddef>
#include <cstdint>

namespace lora_radio {

struct Config {
    float    freqMHz = 906.875f;
    float    bwKhz = 250.0f;
    uint8_t  sf = 11;
    uint8_t  cr = 5;
    uint8_t  syncWord = 0x2B;
    uint16_t preambleLen = 16;
    int8_t   powerDbm = 22;
};

// Brings the module up and leaves it listening. Safe to call again to retune.
bool begin(const Config& c);

bool ok();
float frequencyMHz();

// Set by the DIO1 interrupt: a packet has been received (in RX mode) or the
// transmission has finished (in TX mode).
bool irqFired();

// True while a transmission is in flight.
bool transmitting();

// Copies out a received packet and returns straight to listening. Returns the
// length, 0 if nothing was waiting or the frame failed CRC.
size_t read(uint8_t* buf, size_t cap, float& rssiDbm, float& snrDb);

// Stages a packet and starts transmitting. Returns false if the radio refused
// it; the caller then polls transmitting()/irqFired() and calls finishSend().
bool startSend(const uint8_t* buf, size_t len);

// Completes a transmission (after irqFired) and returns to listening.
void finishSend();

// Abandons a transmission that never reported done, and returns to listening.
void abortSend();

// Back to listening. Called after read()/finishSend(); also recovers the radio
// if it was left in a bad mode.
void listen();

// One channel-activity-detection pass. True if something is already on the air,
// so the caller should back off. Brief (a few symbol times), and always leaves
// the radio listening again.
bool channelBusy();

// Puts the module to sleep (config retained). listen() wakes it. Does NOT touch
// the 3V3 rail — that pin also powers the GPS.
void sleep();

// Rough time-on-air for a packet of `len` bytes at the configured modem
// settings, used for transmit timeouts and the duty-cycle log.
uint32_t airtimeMs(size_t len);

// Last error RadioLib reported, for the diagnostics log.
int lastError();

}  // namespace lora_radio
