// Host filesystem for the sim: "/" of the SD card is a directory on the Mac
// (sim_sd_root()). File behaves like arduino-esp32 2.x's: name() is the
// base name, path() the full path, FILE_WRITE truncates.
#pragma once
#include <Arduino.h>
#include <memory>

#define FILE_READ "r"
#define FILE_WRITE "w"
#define FILE_APPEND "a"

namespace fs {

enum SeekMode { SeekSet = 0, SeekCur = 1, SeekEnd = 2 };

class FileImpl;
class File : public Stream {
public:
    File() {}
    explicit File(std::shared_ptr<FileImpl> p) : p_(std::move(p)) {}
    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t* b, size_t n) override;
    using Print::write;
    int available() override;
    int read() override;
    int peek() override;
    size_t read(uint8_t* b, size_t n);
    void flush() override;
    bool seek(uint32_t pos, SeekMode mode);
    bool seek(uint32_t pos) { return seek(pos, SeekSet); }
    size_t position() const;
    size_t size() const;
    bool setBufferSize(size_t) { return true; }
    void close();
    operator bool() const;
    time_t getLastWrite();
    const char* path() const;
    const char* name() const;
    bool isDirectory() const;
    File openNextFile(const char* mode = FILE_READ);
    void rewindDirectory();
private:
    std::shared_ptr<FileImpl> p_;
};

class FS {
public:
    File open(const char* path, const char* mode = FILE_READ, bool create = false);
    File open(const String& path, const char* mode = FILE_READ, bool create = false) {
        return open(path.c_str(), mode, create); }
    bool exists(const char* path);
    bool exists(const String& p) { return exists(p.c_str()); }
    bool remove(const char* path);
    bool remove(const String& p) { return remove(p.c_str()); }
    bool rename(const char* from, const char* to);
    bool rename(const String& a, const String& b) { return rename(a.c_str(), b.c_str()); }
    bool mkdir(const char* path);
    bool mkdir(const String& p) { return mkdir(p.c_str()); }
    bool rmdir(const char* path);
    bool rmdir(const String& p) { return rmdir(p.c_str()); }
};

}  // namespace fs
using fs::File;
using fs::FS;
using fs::SeekSet;
using fs::SeekCur;
using fs::SeekEnd;

// The card's root on the host, and whether a card is "inserted".
const char* sim_sd_root();
bool sim_sd_present();
