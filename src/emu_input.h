#pragma once

// Emulator-build input: the web page's buttons and screen taps, arriving on
// the same UART1 wire the frames leave on (see epd_compat_emu.cpp for the
// protocol). QEMU models no GPIO, no IO expander and no GT911, so under
// OTP_EMULATOR ui_dashboard reads these instead of the hardware — the SAME
// debounce/long-press logic runs on top, so a held web button opens the power
// dialog exactly like a held physical one.
//
// Only compiled into the t5s3-emu build; the header is safe to include
// anywhere (call sites are #ifdef'd).

#include <stdint.h>

namespace emu_input {

// Drain pending UART1 events into the state below. Called from the UI task's
// input pass, so everything here is single-consumer.
void pump();

// Level states, mirroring digitalRead of the two physical buttons.
bool bootDown();
bool sideDown();

// One-shot, mirroring the GT911 home-key callback.
bool takeHomePress();
bool takeTap(int16_t* x, int16_t* y);

// Current touch state in the panel's portrait coordinates (540x960).
bool touchDown(int16_t* x, int16_t* y);

// Push NMEA bytes in front of the GPS task (single producer: pump()).
void gpsFeed(uint8_t b);

}  // namespace emu_input

// The host->guest event ring, written by the bridge through QEMU's gdbstub.
// A plain global with C linkage so `nm firmware.elf` hands the bridge its
// address; lives in internal DRAM (uncached) so host writes are seen at once.
#define EMU_MAILBOX_SIZE 512

// Emulator: tell the web page which view is showing (map vs other). Defined in
// epd_compat_emu.cpp; called from ui_dashboard.cpp.
void epdc_emit_view(int view);
void epdc_emit_mapstate(int mpp, int trackUp);
struct EmuMailbox {
    volatile uint32_t magic;          // 'OTPM' — the bridge checks it read back
    volatile uint32_t head;           // written by the host
    volatile uint32_t tail;           // written by the guest
    uint8_t buf[EMU_MAILBOX_SIZE];
};
extern "C" EmuMailbox g_emuMailbox;

#ifdef OTP_EMULATOR
#include <Arduino.h>

// Stands in for Serial2 as gps_service's SerialGPS: QEMU's esp32s3 machine
// wires no third serial and its UART model has no loopback, so the receiver's
// bytes arrive through the emulator wire (0xE5) into this ring instead.
// Everything the receiver would ANSWER (config writes, version queries) is
// discarded, which is exactly what a real CASIC module does with a command it
// doesn't recognise. Single-producer (UI task pump) / single-consumer (GPS
// task) ring, index-only synchronisation.
class EmuGpsSerial : public Stream {
public:
    void begin(unsigned long, uint32_t = 0, int8_t = -1, int8_t = -1) {}
    void updateBaudRate(unsigned long) {}
    void setRxBufferSize(size_t) {}

    int available() override { return (int)((head_ - tail_) & MASK); }
    int read() override {
        if (head_ == tail_) return -1;
        const uint8_t b = buf_[tail_ & MASK];
        tail_++;
        return b;
    }
    int peek() override { return head_ == tail_ ? -1 : buf_[tail_ & MASK]; }
    size_t write(uint8_t) override { return 1; }   // the module "hears" and ignores
    using Print::write;

    void feed(uint8_t b) {
        if (((head_ - tail_) & MASK) < MASK) buf_[head_++ & MASK] = b;
    }

private:
    static constexpr uint32_t MASK = 511;
    volatile uint32_t head_ = 0, tail_ = 0;
    uint8_t buf_[512];
};

extern EmuGpsSerial EmuSerialGPS;
#endif
