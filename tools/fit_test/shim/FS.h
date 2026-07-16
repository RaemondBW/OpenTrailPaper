// Host shim for the Arduino FS API, faithful to the ESP32 implementation:
// VFSImpl::open() is a plain fopen(), so the mode string decides whether the
// handle can be read back. Keeping that behaviour here is the whole point —
// opening "w" and then read()ing returns 0 bytes on device, and does so here.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

#define FILE_READ   "r"
#define FILE_WRITE  "w"
#define FILE_APPEND "a"

class File {
public:
    File() = default;
    explicit File(FILE* f, std::string path = {}) : f_(f), path_(std::move(path)) {}

    explicit operator bool() const { return f_ != nullptr; }

    size_t write(const uint8_t* data, size_t len) {
        return f_ ? fwrite(data, 1, len, f_) : 0;
    }
    int read(uint8_t* buf, size_t len) {
        if (!f_) return -1;
        return (int)fread(buf, 1, len, f_);
    }
    bool seek(uint32_t pos) { return f_ && fseek(f_, (long)pos, SEEK_SET) == 0; }
    uint32_t position() const { return f_ ? (uint32_t)ftell(f_) : 0; }
    uint32_t size() const {
        if (!f_) return 0;
        long cur = ftell(f_);
        fseek(f_, 0, SEEK_END);
        long end = ftell(f_);
        fseek(f_, cur, SEEK_SET);
        return (uint32_t)end;
    }
    void flush() { if (f_) fflush(f_); }
    void close() { if (f_) { fclose(f_); f_ = nullptr; } }
    const char* name() const { return path_.c_str(); }

private:
    FILE* f_ = nullptr;
    std::string path_;
};

namespace fs {
class FS {
public:
    explicit FS(std::string root = ".") : root_(std::move(root)) {}
    File open(const char* path, const char* mode = FILE_READ) {
        std::string full = root_ + path;
        FILE* f = fopen(full.c_str(), mode);
        return File(f, full);
    }
    bool exists(const char* path) {
        FILE* f = fopen((root_ + path).c_str(), "r");
        if (f) { fclose(f); return true; }
        return false;
    }
    bool remove(const char* path) { return ::remove((root_ + path).c_str()) == 0; }

private:
    std::string root_;
};
}  // namespace fs
