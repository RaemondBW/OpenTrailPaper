// OTA for the sim: the image the firmware flashes is written to
// <sim dir>/ota.bin; the restart that follows re-execs the sim.
#pragma once
#include <Arduino.h>
#define UPDATE_SIZE_UNKNOWN 0xFFFFFFFF
#define U_FLASH 0
class UpdateClass {
public:
    bool begin(size_t size = UPDATE_SIZE_UNKNOWN, int command = U_FLASH, int ledPin = -1, uint8_t ledOn = LOW,
               const char* label = nullptr);
    size_t write(uint8_t* data, size_t len);
    bool end(bool evenIfRemaining = false);
    void abort();
    uint8_t getError() { return err_; }
    const char* errorString() { return err_ ? "sim update error" : "No Error"; }
    bool hasError() { return err_ != 0; }
    bool isRunning() { return f_ != nullptr; }
    size_t progress() { return written_; }
    size_t size() { return size_; }
private:
    FILE* f_ = nullptr;
    size_t size_ = 0, written_ = 0;
    uint8_t err_ = 0;
};
extern UpdateClass Update;
