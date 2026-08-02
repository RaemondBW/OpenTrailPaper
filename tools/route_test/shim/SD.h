// Host shim: routes.cpp only touches SD for load()/list()/saving uploads. The
// tests drive loadFromMemory(), which never reaches these, so they are stubs
// that fail politely rather than a filesystem emulation.
#pragma once
#include <cstddef>
#include <cstdint>

struct HostFile {
    explicit operator bool() const { return false; }
    int  read(uint8_t*, size_t) { return 0; }
    size_t size() const { return 0; }
    void close() {}
    bool isDirectory() { return false; }
    const char* name() { return ""; }
    HostFile openNextFile() { return HostFile(); }
    size_t write(const uint8_t*, size_t) { return 0; }
};

struct HostSD {
    HostFile open(const char*, const char* = "r") { return HostFile(); }
    bool exists(const char*) { return false; }
    bool mkdir(const char*) { return true; }
    bool remove(const char*) { return true; }
};
static HostSD SD;
using File = HostFile;
#define FILE_READ  "r"
#define FILE_WRITE "w"
