#include "rtc_clock.h"

#include <Arduino.h>
#include <Wire.h>

#include "i2c_bus.h"

namespace {

constexpr uint8_t PCF8563_ADDR = 0x51;
constexpr uint8_t REG_SECONDS  = 0x02;   // VL flag in bit 7
constexpr uint8_t VL_FLAG      = 0x80;   // seconds bit 7: clock integrity lost

uint8_t bcd2dec(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
uint8_t dec2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// Days-from-civil (Howard Hinnant): calendar UTC -> unix epoch, no libc TZ.
time_t toUnix(int y, unsigned m, unsigned d, unsigned hh, unsigned mm, unsigned ss) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long days = era * 146097L + static_cast<long>(doe) - 719468L;
    return static_cast<time_t>(days) * 86400 + hh * 3600 + mm * 60 + ss;
}

}  // namespace

namespace rtc_clock {

bool begin() {
    i2cLock();
    Wire.beginTransmission(PCF8563_ADDR);
    bool ok = (Wire.endTransmission() == 0);
    i2cUnlock();
    return ok;
}

bool read(time_t& out) {
    uint8_t r[7];
    i2cLock();
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(REG_SECONDS);
    if (Wire.endTransmission(false) != 0) { i2cUnlock(); return false; }
    if (Wire.requestFrom((int)PCF8563_ADDR, 7) != 7) { i2cUnlock(); return false; }
    for (uint8_t& b : r) b = Wire.read();
    i2cUnlock();

    if (r[0] & VL_FLAG) return false;   // clock integrity not guaranteed

    int sec  = bcd2dec(r[0] & 0x7F);
    int min  = bcd2dec(r[1] & 0x7F);
    int hour = bcd2dec(r[2] & 0x3F);
    int day  = bcd2dec(r[3] & 0x3F);
    // r[4] = weekday (unused); r[5] month has the century flag in bit 7.
    int mon  = bcd2dec(r[5] & 0x1F);
    int year = 2000 + bcd2dec(r[6]);

    if (mon < 1 || mon > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 59 || year < 2025 || year > 2099) {
        return false;
    }
    out = toUnix(year, mon, day, hour, min, sec);
    return true;
}

void write(time_t utc) {
    struct tm t;
    gmtime_r(&utc, &t);
    i2cLock();
    Wire.beginTransmission(PCF8563_ADDR);
    Wire.write(REG_SECONDS);
    Wire.write(dec2bcd(t.tm_sec) & 0x7F);   // also clears VL
    Wire.write(dec2bcd(t.tm_min));
    Wire.write(dec2bcd(t.tm_hour));
    Wire.write(dec2bcd(t.tm_mday));
    Wire.write(dec2bcd(t.tm_wday));
    Wire.write(dec2bcd(t.tm_mon + 1));      // century bit left 0 -> 20xx
    Wire.write(dec2bcd((uint8_t)(t.tm_year + 1900 - 2000)));
    Wire.endTransmission();
    i2cUnlock();
}

}  // namespace rtc_clock
