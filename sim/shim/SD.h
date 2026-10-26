#pragma once
#include <FS.h>
#include <SPI.h>

typedef enum { CARD_NONE, CARD_MMC, CARD_SD, CARD_SDHC, CARD_UNKNOWN } sdcard_type_t;

class SDFS : public fs::FS {
public:
    bool begin(uint8_t ssPin = 0, SPIClass& spi = SPI, uint32_t frequency = 4000000,
               const char* mountpoint = "/sd", uint8_t max_files = 5, bool format_if_empty = false);
    void end();
    sdcard_type_t cardType();
    uint64_t cardSize();
    size_t numSectors() { return cardSize() / 512; }
    size_t sectorSize() { return 512; }
    uint64_t totalBytes() { return cardSize(); }
    uint64_t usedBytes();
    bool readRAW(uint8_t*, uint32_t) { return false; }
    bool writeRAW(uint8_t*, uint32_t) { return false; }
private:
    bool mounted_ = false;
};
extern SDFS SD;
