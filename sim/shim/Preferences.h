// NVS for the sim: one JSON-free key/value file per namespace under the
// sim's nvs directory, written through on every put (the device's NVS
// survives reboots; so does this, until the test deletes the directory).
#pragma once
#include <Arduino.h>
class Preferences {
public:
    bool begin(const char* ns, bool readOnly = false, const char* partition = nullptr);
    void end();
    bool clear();
    bool remove(const char* key);
    bool isKey(const char* key);
    size_t putChar(const char* k, int8_t v) { return putRaw(k, &v, sizeof v); }
    size_t putUChar(const char* k, uint8_t v) { return putRaw(k, &v, sizeof v); }
    size_t putShort(const char* k, int16_t v) { return putRaw(k, &v, sizeof v); }
    size_t putUShort(const char* k, uint16_t v) { return putRaw(k, &v, sizeof v); }
    size_t putInt(const char* k, int32_t v) { return putRaw(k, &v, sizeof v); }
    size_t putUInt(const char* k, uint32_t v) { return putRaw(k, &v, sizeof v); }
    size_t putLong(const char* k, int32_t v) { return putRaw(k, &v, sizeof v); }
    size_t putULong(const char* k, uint32_t v) { return putRaw(k, &v, sizeof v); }
    size_t putLong64(const char* k, int64_t v) { return putRaw(k, &v, sizeof v); }
    size_t putULong64(const char* k, uint64_t v) { return putRaw(k, &v, sizeof v); }
    size_t putFloat(const char* k, float v) { return putRaw(k, &v, sizeof v); }
    size_t putDouble(const char* k, double v) { return putRaw(k, &v, sizeof v); }
    size_t putBool(const char* k, bool v) { uint8_t b = v; return putRaw(k, &b, 1); }
    size_t putString(const char* k, const char* v) { return putRaw(k, v, strlen(v) + 1); }
    size_t putString(const char* k, const String& v) { return putString(k, v.c_str()); }
    size_t putBytes(const char* k, const void* v, size_t n) { return putRaw(k, v, n); }
    int8_t getChar(const char* k, int8_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    uint8_t getUChar(const char* k, uint8_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    int16_t getShort(const char* k, int16_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    uint16_t getUShort(const char* k, uint16_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    int32_t getInt(const char* k, int32_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    uint32_t getUInt(const char* k, uint32_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    int32_t getLong(const char* k, int32_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    uint32_t getULong(const char* k, uint32_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    int64_t getLong64(const char* k, int64_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    uint64_t getULong64(const char* k, uint64_t d = 0) { getRaw(k, &d, sizeof d); return d; }
    float getFloat(const char* k, float d = 0) { getRaw(k, &d, sizeof d); return d; }
    double getDouble(const char* k, double d = 0) { getRaw(k, &d, sizeof d); return d; }
    bool getBool(const char* k, bool d = false) { uint8_t b = d; getRaw(k, &b, 1); return b != 0; }
    size_t getString(const char* k, char* out, size_t max);
    String getString(const char* k, const String& d = String());
    size_t getBytesLength(const char* k);
    size_t getBytes(const char* k, void* out, size_t max);
    size_t freeEntries() { return 400; }
private:
    size_t putRaw(const char* k, const void* v, size_t n);
    bool getRaw(const char* k, void* out, size_t n);
    std::string ns_;
    bool open_ = false;
};
