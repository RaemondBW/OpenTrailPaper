// No I2C devices answer in the sim: every probe NACKs, so the firmware takes
// its "not fitted" paths (no baro, no touch, no fuel gauge chip).
#pragma once
#include <Arduino.h>
class TwoWire : public Stream {
public:
    explicit TwoWire(int bus = 0) { (void)bus; }
    bool begin(int sda = -1, int scl = -1, uint32_t freq = 0) { (void)sda; (void)scl; (void)freq; return true; }
    bool end() { return true; }
    void setClock(uint32_t) {}
    void setTimeOut(uint16_t) {}
    void beginTransmission(uint8_t) {}
    uint8_t endTransmission(bool stop = true) { (void)stop; return 2; }
    uint8_t requestFrom(uint8_t, size_t n, bool stop = true) { (void)n; (void)stop; return 0; }
    uint8_t requestFrom(int a, int n) { (void)a; (void)n; return 0; }
    size_t write(uint8_t) override { return 1; }
    size_t write(const uint8_t*, size_t n) override { return n; }
    using Print::write;
    int available() override { return 0; }
    int read() override { return -1; }
    int peek() override { return -1; }
};
extern TwoWire Wire;
extern TwoWire Wire1;
