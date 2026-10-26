// The SD card and NVS on the host. The card is a directory (`--sd`, default
// <dir>/sd); a test can pull it out (`sd present=false`). NVS is one file per
// key under <dir>/nvs/<namespace>/, so it survives reboots like the device's.
#include <SD.h>
#include <Preferences.h>
#include <ff.h>

#include "sim.h"
#include "sim_clock.h"

#include <algorithm>
#include <atomic>
#include <dirent.h>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace {
// Whether a card is in the slot. A pulled card stays pulled across reboots
// (a marker file in the device directory), as it would on the board.
std::string removedMarker() { return sim::opts.dir + "/sd-removed"; }
std::atomic<bool> g_present{true};
std::once_flag g_presentInit;
void initPresent() {
    std::call_once(g_presentInit, [] {
        struct stat st;
        g_present = stat(removedMarker().c_str(), &st) != 0;
    });
}

std::string hostPath(const char* cardPath) {
    std::string p = cardPath ? cardPath : "/";
    if (p.rfind("/sd/", 0) == 0) p = p.substr(3);   // a POSIX path through the VFS mount
    else if (p == "/sd") p = "/";
    if (p.empty() || p[0] != '/') p = "/" + p;
    return std::string(sim_sd_root()) + p;
}

bool isDir(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

void mkdirs(const std::string& p) {
    for (size_t i = 1; i < p.size(); ++i)
        if (p[i] == '/') mkdir(p.substr(0, i).c_str(), 0755);
    mkdir(p.c_str(), 0755);
}
}  // namespace

const char* sim_sd_root() {
    static std::string root = [] {
        std::string r = sim::opts.sdDir.empty() ? sim::opts.dir + "/sd" : sim::opts.sdDir;
        mkdirs(r);
        return r;
    }();
    return root.c_str();
}
bool sim_sd_present() { initPresent(); return g_present.load(); }
bool sim::sdPresent() { initPresent(); return g_present.load(); }
void sim::setSdPresent(bool on) {
    initPresent();
    g_present = on;
    if (on) unlink(removedMarker().c_str());
    else if (FILE* f = fopen(removedMarker().c_str(), "w")) fclose(f);
}

// The SD card shares its SPI bus with the LoRa radio, and every card access
// must hold sdLock() (sd_bus.h). On the board an unlocked one interleaves with
// a radio transfer and wedges the card; here nothing would go wrong, so the
// sim says so instead: once firmware tasks are running, a card call from a
// task that doesn't hold the SD mutex is logged once per caller.
extern SemaphoreHandle_t g_sdMutex;
namespace {
void checkBus(const char* op, const char* path, void* caller) {
    if (!g_sdMutex || !sim_tasks_started() || sim_sem_held_by_me(g_sdMutex)) return;
    std::string fn = sim::symbolName(caller);
    sim::warnOnce(std::string("sd-unlocked:") + fn + ":" + op,
                  std::string("[sim] SD ACCESS WITHOUT sdLock: ") + op + " " + (path ? path : "") + " from " + fn +
                      " (task " + sim_task_name() + ")");
}
#define CHECK_BUS(op, path) checkBus(op, path, __builtin_return_address(0))

// A slow card: every open, read and write costs this much device time, as a
// 4 MHz SPI card's seconds of finalize-and-flush do on the board.
std::atomic<int> g_latencyMs{0};
void cardTime() {
    if (int ms = g_latencyMs.load()) sim_clock::sleepMs(ms);
}
}  // namespace
void sim::setSdLatency(int ms) { g_latencyMs = ms; }

namespace fs {

class FileImpl {
public:
    std::string cardPath, host, base;
    FILE* f = nullptr;
    bool dir = false;
    std::vector<std::string> entries;
    size_t next = 0;
    ~FileImpl() { if (f) fclose(f); }
};

size_t File::write(const uint8_t* b, size_t n) {
    if (!p_ || !p_->f || !g_present) return 0;
    CHECK_BUS("write", p_->cardPath.c_str());
    cardTime();
    return fwrite(b, 1, n, p_->f);
}
int File::available() {
    if (!p_ || !p_->f) return 0;
    long pos = ftell(p_->f);
    return (int)(size() - (size_t)pos);
}
int File::read() {
    uint8_t c;
    return read(&c, 1) == 1 ? c : -1;
}
int File::peek() {
    if (!p_ || !p_->f) return -1;
    int c = fgetc(p_->f);
    if (c != EOF) ungetc(c, p_->f);
    return c == EOF ? -1 : c;
}
size_t File::read(uint8_t* b, size_t n) {
    if (!p_ || !p_->f || !g_present) return 0;
    CHECK_BUS("read", p_->cardPath.c_str());
    cardTime();
    if (!sim::opts.sdCrashOnRead.empty() && sim::opts.sdCrashOnRead == p_->cardPath)
        sim::reboot("watchdog: --sd-crash-on-read");
    return fread(b, 1, n, p_->f);
}
void File::flush() { if (p_ && p_->f) fflush(p_->f); }
bool File::seek(uint32_t pos, SeekMode mode) {
    if (!p_ || !p_->f) return false;
    int whence = mode == SeekSet ? SEEK_SET : mode == SeekCur ? SEEK_CUR : SEEK_END;
    return fseek(p_->f, (long)pos, whence) == 0;
}
size_t File::position() const { return p_ && p_->f ? (size_t)ftell(p_->f) : 0; }
size_t File::size() const {
    if (!p_) return 0;
    if (p_->f) fflush(p_->f);
    struct stat st;
    return stat(p_->host.c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}
void File::close() { p_.reset(); }
File::operator bool() const { return p_ && (p_->f || p_->dir); }
time_t File::getLastWrite() {
    struct stat st;
    return p_ && stat(p_->host.c_str(), &st) == 0 ? st.st_mtime : 0;
}
const char* File::path() const { return p_ ? p_->cardPath.c_str() : ""; }
const char* File::name() const { return p_ ? p_->base.c_str() : ""; }
bool File::isDirectory() const { return p_ && p_->dir; }
File File::openNextFile(const char* mode) {
    if (!p_ || !p_->dir || p_->next >= p_->entries.size()) return File();
    std::string child = p_->cardPath;
    if (child.empty() || child.back() != '/') child += "/";
    child += p_->entries[p_->next++];
    return SD.open(child.c_str(), mode);
}
void File::rewindDirectory() { if (p_) p_->next = 0; }

File FS::open(const char* path, const char* mode, bool) {
    if (!g_present) return File();
    CHECK_BUS("open", path);
    cardTime();
    auto impl = std::make_shared<FileImpl>();
    impl->cardPath = path;
    impl->host = hostPath(path);
    const char* slash = strrchr(path, '/');
    impl->base = slash ? slash + 1 : path;
    std::string m = mode ? mode : "r";
    if (m == "r" && isDir(impl->host)) {
        impl->dir = true;
        if (DIR* d = opendir(impl->host.c_str())) {
            while (dirent* e = readdir(d))
                if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) impl->entries.push_back(e->d_name);
            closedir(d);
        }
        std::sort(impl->entries.begin(), impl->entries.end());
        return File(impl);
    }
    std::string fm = m == "r" ? "rb" : m == "w" ? "wb" : m == "a" ? "ab" : m == "r+" ? "r+b"
                   : m == "w+" ? "w+b" : m == "a+" ? "a+b" : "rb";
    impl->f = fopen(impl->host.c_str(), fm.c_str());
    if (!impl->f) return File();
    return File(impl);
}
bool FS::exists(const char* path) {
    CHECK_BUS("exists", path);
    struct stat st;
    return g_present && stat(hostPath(path).c_str(), &st) == 0;
}
bool FS::remove(const char* path) {
    CHECK_BUS("remove", path);
    return g_present && unlink(hostPath(path).c_str()) == 0;
}
bool FS::rename(const char* from, const char* to) {
    CHECK_BUS("rename", from);
    return g_present && ::rename(hostPath(from).c_str(), hostPath(to).c_str()) == 0;
}
bool FS::mkdir(const char* path) {
    return g_present && (::mkdir(hostPath(path).c_str(), 0755) == 0 || isDir(hostPath(path)));
}
bool FS::rmdir(const char* path) { return g_present && ::rmdir(hostPath(path).c_str()) == 0; }

}  // namespace fs

SDFS SD;

bool SDFS::begin(uint8_t, SPIClass&, uint32_t, const char*, uint8_t, bool) {
    initPresent();
    // A card still coming up after power-on fails the init handshake, as a
    // real 32 GB SDHC did for its first ~1.5-2 s (investigations/sd-boot-mount).
    mounted_ = g_present.load() && millis() >= (uint32_t)sim::opts.sdReadyMs;
    return mounted_;
}
void SDFS::end() { mounted_ = false; }
sdcard_type_t SDFS::cardType() { return g_present ? CARD_SDHC : CARD_NONE; }
uint64_t SDFS::cardSize() { return g_present ? 32ull << 30 : 0; }
uint64_t SDFS::usedBytes() { return g_present ? 64ull << 20 : 0; }

// ---- FatFs directory scan -------------------------------------------------------

extern "C" FRESULT f_opendir(FF_DIR* dp, const char* path) {
    if (!g_present) return FR_DISK_ERR;
    CHECK_BUS("f_opendir", path);
    const char* p = strchr(path, ':');
    if (!p || path[0] != '0') return FR_INVALID_DRIVE;
    DIR* d = opendir(hostPath(p + 1).c_str());
    if (!d) return FR_NO_PATH;
    dp->d = d;
    return FR_OK;
}
extern "C" FRESULT f_readdir(FF_DIR* dp, FILINFO* fi) {
    CHECK_BUS("f_readdir", "");
    DIR* d = (DIR*)dp->d;
    for (;;) {
        dirent* e = readdir(d);
        if (!e) { fi->fname[0] = 0; return FR_OK; }
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        snprintf(fi->fname, sizeof fi->fname, "%s", e->d_name);
        fi->fattrib = e->d_type == DT_DIR ? AM_DIR : 0;
        fi->fsize = 0;
        struct stat st;
        if (e->d_type != DT_DIR && fstatat(dirfd(d), e->d_name, &st, 0) == 0)
            fi->fsize = (uint32_t)st.st_size;
        return FR_OK;
    }
}
extern "C" FRESULT f_closedir(FF_DIR* dp) {
    if (dp->d) closedir((DIR*)dp->d);
    dp->d = nullptr;
    return FR_OK;
}

// ---- NVS -------------------------------------------------------------------

namespace {
std::mutex g_nvs;
std::string nvsPath(const std::string& ns, const char* key) {
    return sim::opts.dir + "/nvs/" + ns + (key ? std::string("/") + key : "");
}
}

bool Preferences::begin(const char* ns, bool, const char*) {
    ns_ = ns;
    open_ = true;
    mkdirs(nvsPath(ns_, nullptr));
    return true;
}
void Preferences::end() { open_ = false; }
bool Preferences::clear() {
    std::lock_guard<std::mutex> lk(g_nvs);
    std::string d = nvsPath(ns_, nullptr);
    if (DIR* dir = opendir(d.c_str())) {
        while (dirent* e = readdir(dir))
            if (e->d_type == DT_REG) unlink((d + "/" + e->d_name).c_str());
        closedir(dir);
    }
    return true;
}
bool Preferences::remove(const char* key) {
    std::lock_guard<std::mutex> lk(g_nvs);
    return unlink(nvsPath(ns_, key).c_str()) == 0;
}
bool Preferences::isKey(const char* key) {
    struct stat st;
    return stat(nvsPath(ns_, key).c_str(), &st) == 0;
}
size_t Preferences::putRaw(const char* k, const void* v, size_t n) {
    if (!open_) return 0;
    std::lock_guard<std::mutex> lk(g_nvs);
    std::string p = nvsPath(ns_, k), tmp = p + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) return 0;
    fwrite(v, 1, n, f);
    fclose(f);
    ::rename(tmp.c_str(), p.c_str());
    return n;
}
bool Preferences::getRaw(const char* k, void* out, size_t n) {
    if (!open_) return false;
    std::lock_guard<std::mutex> lk(g_nvs);
    FILE* f = fopen(nvsPath(ns_, k).c_str(), "rb");
    if (!f) return false;
    std::vector<uint8_t> buf(n + 1);
    size_t got = fread(buf.data(), 1, n + 1, f);
    fclose(f);
    if (got != n) return false;   // a type mismatch reads as absent, as on NVS
    memcpy(out, buf.data(), n);
    return true;
}
size_t Preferences::getBytesLength(const char* k) {
    struct stat st;
    return stat(nvsPath(ns_, k).c_str(), &st) == 0 ? (size_t)st.st_size : 0;
}
size_t Preferences::getBytes(const char* k, void* out, size_t max) {
    size_t n = getBytesLength(k);
    if (!n || n > max) return 0;
    return getRaw(k, out, n) ? n : 0;
}
size_t Preferences::getString(const char* k, char* out, size_t max) {
    size_t n = getBytesLength(k);
    if (!n || n > max) return 0;
    if (!getRaw(k, out, n)) return 0;
    out[n - 1] = 0;
    return n;
}
String Preferences::getString(const char* k, const String& d) {
    size_t n = getBytesLength(k);
    if (!n) return d;
    std::vector<char> buf(n);
    if (!getRaw(k, buf.data(), n)) return d;
    buf[n - 1] = 0;
    return String(buf.data());
}
