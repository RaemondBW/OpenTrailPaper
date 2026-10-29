// Host Arduino core for the firmware sim: timing from the sim clock, Serial
// as the sim console (stdout + an injectable input queue), Serial2 as the GPS
// UART (the sim's GPS module reads what the firmware writes and feeds NMEA).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <algorithm>
#include <string>
#include <functional>
#include "sim_rtos.h"
#include "esp_sim.h"
#include "driver/gpio.h"

using std::min;
using std::max;
typedef uint8_t byte;
typedef bool boolean;

#define HIGH 1
#define LOW 0
#define INPUT 0x01
#define OUTPUT 0x03
#define INPUT_PULLUP 0x05
#define INPUT_PULLDOWN 0x09
#define OUTPUT_OPEN_DRAIN 0x13
#define RISING 0x01
#define FALLING 0x02
#define CHANGE 0x03
#define PI 3.1415926535897932384626433832795
#define HALF_PI 1.5707963267948966192313216916398
#define TWO_PI 6.283185307179586476925286766559
#define DEG_TO_RAD 0.017453292519943295769236907684886
#define RAD_TO_DEG 57.295779513082320876798154814105
#define SERIAL_8N1 0x800001c
#define F(s) (s)
#define PROGMEM
#define pgm_read_byte(p) (*(const uint8_t*)(p))
#define pgm_read_word(p) (*(const uint16_t*)(p))
#define pgm_read_dword(p) (*(const uint32_t*)(p))
#define digitalPinToInterrupt(p) (p)
#ifndef constrain
#define constrain(amt, low, high) ((amt) < (low) ? (low) : ((amt) > (high) ? (high) : (amt)))
#endif
#define radians(deg) ((deg) * DEG_TO_RAD)
#define degrees(rad) ((rad) * RAD_TO_DEG)
#define sq(x) ((x) * (x))
#define bitRead(value, bit) (((value) >> (bit)) & 0x01)
#define bitSet(value, bit) ((value) |= (1UL << (bit)))
#define bitClear(value, bit) ((value) &= ~(1UL << (bit)))
#define lowByte(w) ((uint8_t)((w) & 0xff))
#define highByte(w) ((uint8_t)((w) >> 8))

uint32_t millis();
uint32_t micros();
void delay(uint32_t ms);
void delayMicroseconds(uint32_t us);
void yield();
inline void pinMode(int, int) {}
void digitalWrite(int pin, int v);
int digitalRead(int pin);
inline int analogRead(int) { return 0; }
inline uint32_t analogReadMilliVolts(int) { return 0; }
void analogWrite(int pin, int duty);   // recorded per pin (sim::pinDuty)
inline void attachInterrupt(int, void (*)(void), int) {}
inline void detachInterrupt(int) {}
inline long random(long hi) { return hi > 0 ? (long)(esp_random() % (uint32_t)hi) : 0; }
inline long random(long lo, long hi) { return hi > lo ? lo + random(hi - lo) : lo; }
inline void randomSeed(unsigned long) {}
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min; }
inline uint32_t getCpuFrequencyMhz() { return 80; }
inline bool setCpuFrequencyMhz(uint32_t) { return true; }
inline uint32_t temperatureRead() { return 30; }
inline void ledcAttach(int, int, int) {}
inline void ledcWrite(int, int) {}
inline bool ledcAttachPin(int, int) { return true; }
inline void ledcSetup(int, int, int) {}

class String {
public:
    String() {}
    String(const char* s) : s_(s ? s : "") {}
    String(const std::string& s) : s_(s) {}
    String(char c) : s_(1, c) {}
    String(int v) : s_(std::to_string(v)) {}
    String(unsigned v) : s_(std::to_string(v)) {}
    String(long v) : s_(std::to_string(v)) {}
    String(unsigned long v) : s_(std::to_string(v)) {}
    String(double v, int dec = 2) { char b[48]; snprintf(b, sizeof b, "%.*f", dec, v); s_ = b; }
    const char* c_str() const { return s_.c_str(); }
    size_t length() const { return s_.size(); }
    bool isEmpty() const { return s_.empty(); }
    char operator[](size_t i) const { return i < s_.size() ? s_[i] : 0; }
    char charAt(size_t i) const { return (*this)[i]; }
    String& operator+=(const String& o) { s_ += o.s_; return *this; }
    String& operator+=(const char* o) { s_ += o ? o : ""; return *this; }
    String& operator+=(char c) { s_ += c; return *this; }
    friend String operator+(const String& a, const String& b) { return String(a.s_ + b.s_); }
    friend String operator+(const String& a, const char* b) { return String(a.s_ + (b ? b : "")); }
    friend String operator+(const char* a, const String& b) { return String(std::string(a ? a : "") + b.s_); }
    bool operator==(const String& o) const { return s_ == o.s_; }
    bool operator==(const char* o) const { return s_ == (o ? o : ""); }
    bool operator!=(const String& o) const { return s_ != o.s_; }
    bool equals(const String& o) const { return s_ == o.s_; }
    bool startsWith(const String& p) const { return s_.compare(0, p.s_.size(), p.s_) == 0; }
    bool endsWith(const String& p) const {
        return s_.size() >= p.s_.size() && s_.compare(s_.size() - p.s_.size(), p.s_.size(), p.s_) == 0; }
    int indexOf(char c, size_t from = 0) const { auto i = s_.find(c, from); return i == std::string::npos ? -1 : (int)i; }
    int indexOf(const String& t, size_t from = 0) const { auto i = s_.find(t.s_, from); return i == std::string::npos ? -1 : (int)i; }
    String substring(size_t a) const { return a < s_.size() ? String(s_.substr(a)) : String(); }
    String substring(size_t a, size_t b) const { return a < s_.size() && b > a ? String(s_.substr(a, b - a)) : String(); }
    void trim() { size_t a = s_.find_first_not_of(" \t\r\n"); size_t b = s_.find_last_not_of(" \t\r\n");
                  s_ = a == std::string::npos ? "" : s_.substr(a, b - a + 1); }
    void toLowerCase() { for (auto& c : s_) c = (char)tolower((unsigned char)c); }
    void toUpperCase() { for (auto& c : s_) c = (char)toupper((unsigned char)c); }
    long toInt() const { return strtol(s_.c_str(), nullptr, 10); }
    float toFloat() const { return strtof(s_.c_str(), nullptr); }
    void reserve(size_t n) { s_.reserve(n); }
    operator const char*() const = delete;
    const std::string& std() const { return s_; }
private:
    std::string s_;
};

class Print {
public:
    virtual ~Print() {}
    virtual size_t write(uint8_t c) = 0;
    virtual size_t write(const uint8_t* b, size_t n) { size_t k = 0; while (k < n) write(b[k++]); return n; }
    size_t write(const char* s) { return s ? write((const uint8_t*)s, strlen(s)) : 0; }
    size_t write(const char* s, size_t n) { return write((const uint8_t*)s, n); }
    size_t printf(const char* fmt, ...) __attribute__((format(printf, 2, 3))) {
        char stackBuf[512]; va_list ap; va_start(ap, fmt);
        va_list ap2; va_copy(ap2, ap);
        int n = vsnprintf(stackBuf, sizeof stackBuf, fmt, ap); va_end(ap);
        if (n < 0) { va_end(ap2); return 0; }
        if ((size_t)n < sizeof stackBuf) { va_end(ap2); return write((const uint8_t*)stackBuf, n); }
        std::string big((size_t)n + 1, 0); vsnprintf(&big[0], big.size(), fmt, ap2); va_end(ap2);
        return write((const uint8_t*)big.data(), n);
    }
    size_t print(const char* s) { return write(s); }
    size_t print(const String& s) { return write(s.c_str()); }
    size_t print(char c) { return write((uint8_t)c); }
    size_t print(int v) { return printf("%d", v); }
    size_t print(unsigned v) { return printf("%u", v); }
    size_t print(long v) { return printf("%ld", v); }
    size_t print(unsigned long v) { return printf("%lu", v); }
    size_t print(double v, int d = 2) { return printf("%.*f", d, v); }
    size_t println() { return write("\r\n"); }
    template <typename T> size_t println(T v) { size_t n = print(v); return n + println(); }
    size_t println(double v, int d) { size_t n = print(v, d); return n + println(); }
    virtual void flush() {}
};

class Stream : public Print {
public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int peek() = 0;
    void setTimeout(unsigned long ms) { timeoutMs_ = ms; }
    String readString() { std::string s; int c; uint32_t t0 = millis();
        while (millis() - t0 < timeoutMs_) { if ((c = read()) >= 0) { s += (char)c; t0 = millis(); } else delay(1); }
        return String(s); }
    String readStringUntil(char term) { std::string s; int c; uint32_t t0 = millis();
        while (millis() - t0 < timeoutMs_) { if ((c = read()) >= 0) { if (c == term) break; s += (char)c; } else delay(1); }
        return String(s); }
    size_t readBytes(uint8_t* b, size_t n) { size_t k = 0; uint32_t t0 = millis(); int c;
        while (k < n && millis() - t0 < timeoutMs_) { if ((c = read()) >= 0) b[k++] = (uint8_t)c; else delay(1); }
        return k; }
protected:
    unsigned long timeoutMs_ = 1000;
};

// A UART: what the firmware writes goes to `sink`; bytes pushed with
// sim_feed() are what it reads.
class HardwareSerial : public Stream {
public:
    explicit HardwareSerial(int port);
    void begin(unsigned long baud, uint32_t config = SERIAL_8N1, int rx = -1, int tx = -1,
               bool invert = false, unsigned long timeout = 20000UL, uint8_t rxfifo = 112);
    void end() {}
    void updateBaudRate(unsigned long baud);
    unsigned long baudRate() const { return baud_; }
    int available() override;
    int read() override;
    int peek() override;
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* b, size_t n) override;
    using Print::write;
    int availableForWrite() { return 4096; }
    void flush() override {}
    size_t setRxBufferSize(size_t n) { return n; }
    size_t setTxBufferSize(size_t n) { return n; }
    bool setRxTimeout(uint8_t) { return true; }
    bool setRxFIFOFull(uint8_t) { return true; }
    void setDebugOutput(bool) {}
    void onReceive(std::function<void(void)> cb, bool onlyOnTimeout = false) { onReceive_ = cb; (void)onlyOnTimeout; }
    void onReceiveError(std::function<void(int)> cb) { (void)cb; }
    operator bool() const { return true; }
    // sim side
    void sim_feed(const uint8_t* b, size_t n);
    std::function<void(const uint8_t*, size_t)> sink;
    int port() const { return port_; }
private:
    int port_;
    unsigned long baud_ = 115200;
    std::function<void(void)> onReceive_;
    struct Impl; Impl* impl_;
};
extern HardwareSerial Serial;
extern HardwareSerial Serial1;
extern HardwareSerial Serial2;
using HWCDC = HardwareSerial;
