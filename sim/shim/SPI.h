#pragma once
#include <Arduino.h>
#define SPI_MODE0 0
#define MSBFIRST 1
class SPISettings { public: SPISettings() {} SPISettings(uint32_t, uint8_t, uint8_t) {} };
class SPIClass {
public:
    explicit SPIClass(int bus = 0) { (void)bus; }
    void begin(int sck = -1, int miso = -1, int mosi = -1, int ss = -1) { (void)sck; (void)miso; (void)mosi; (void)ss; }
    void end() {}
    void beginTransaction(SPISettings) {}
    void endTransaction() {}
    uint8_t transfer(uint8_t) { return 0xFF; }
    void transfer(void*, size_t) {}
    void setFrequency(uint32_t) {}
};
extern SPIClass SPI;
#define FSPI 0
#define HSPI 1
