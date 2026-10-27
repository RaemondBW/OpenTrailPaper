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

// ---- The card's capacity ------------------------------------------------------
//
// A FAT32 card of --sd-size-mb (default 32 GB): space is allocated in whole
// clusters, a directory's entries take clusters of their own, and the FATs and
// reserved sectors come off the top. Usage is counted from <dir>/sd when the
// card is mounted (and whenever usedBytes() is asked), then kept up to date by
// every write, truncate, remove and mkdir through this file layer. A write
// that needs a cluster the card doesn't have is cut short where the last free
// cluster ends, as FatFs's f_write is: the caller gets the short count (0 and
// ENOSPC when nothing fit), never a crash.
namespace {
struct Card {
    std::recursive_mutex m;
    uint64_t sizeBytes = 0;   // the raw card
    uint32_t cluster = 0;     // bytes per cluster
    uint64_t clusters = 0;    // data clusters
    uint64_t used = 0;        // of which in use
    bool counted = false;
} g_card;

uint64_t clustersFor(uint64_t bytes) { return (bytes + g_card.cluster - 1) / g_card.cluster; }

void cardConfigure() {   // under g_card.m
    if (g_card.cluster) return;
    g_card.sizeBytes = sim::opts.sdSizeMb ? (uint64_t)sim::opts.sdSizeMb << 20 : 32ull << 30;
    uint32_t kb = sim::opts.sdClusterKb;
    if (!kb) kb = g_card.sizeBytes >= (2ull << 30) ? 32 : 16;   // SD Formatter's FAT32 / small-card choices
    g_card.cluster = kb * 1024;
    // FAT32: 32 reserved sectors, then two FATs of 4 bytes per cluster.
    const uint64_t avail = g_card.sizeBytes > 32 * 512 ? g_card.sizeBytes - 32 * 512 : 0;
    g_card.clusters = avail / (g_card.cluster + 8);
}

// Clusters taken by everything under host directory `p`, the directory's own
// entries included (one short + ceil(len/13) long-name entries of 32 bytes per
// name, as FatFs writes them).
uint64_t countTree(const std::string& p) {
    uint64_t n = 0, dirBytes = 0;
    if (DIR* d = opendir(p.c_str())) {
        while (dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            dirBytes += 32 * (1 + (strlen(e->d_name) + 12) / 13);
            if (e->d_type == DT_DIR) {
                n += countTree(p + "/" + e->d_name);
            } else {
                struct stat st;
                if (fstatat(dirfd(d), e->d_name, &st, 0) == 0) n += clustersFor((uint64_t)st.st_size);
            }
        }
        closedir(d);
    }
    uint64_t dc = clustersFor(dirBytes);
    return n + (dc ? dc : 1);
}

void cardCount() {   // under g_card.m
    cardConfigure();
    g_card.used = countTree(sim_sd_root());
    g_card.counted = true;
}

void cardEnsure() {   // under g_card.m
    cardConfigure();
    if (!g_card.counted) cardCount();
}

uint64_t cardFree() {   // clusters; under g_card.m
    return g_card.used >= g_card.clusters ? 0 : g_card.clusters - g_card.used;
}

void cardRelease(uint64_t bytes) {   // under g_card.m
    uint64_t c = clustersFor(bytes);
    g_card.used = g_card.used > c ? g_card.used - c : 0;
}

uint64_t hostSize(const std::string& host) {
    struct stat st;
    return stat(host.c_str(), &st) == 0 ? (uint64_t)st.st_size : 0;
}

// How many of `n` bytes written at `pos` into a file now `size` long fit on
// the card; books the clusters they take and moves `size` on.
size_t cardReserve(uint64_t& size, uint64_t pos, size_t n) {
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    cardEnsure();
    const uint64_t have = clustersFor(size);
    uint64_t end = std::max<uint64_t>(size, pos + n);
    const uint64_t need = clustersFor(end) - have;
    size_t allowed = n;
    if (need > cardFree()) {
        const uint64_t maxEnd = (have + cardFree()) * g_card.cluster;
        allowed = maxEnd > pos ? (size_t)std::min<uint64_t>(n, maxEnd - pos) : 0;
        end = std::max<uint64_t>(size, pos + allowed);
    }
    g_card.used += clustersFor(end) - have;
    size = end;
    return allowed;
}
}  // namespace

void sim::setSdSizeMb(uint64_t mb, uint32_t clusterKb) {
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    opts.sdSizeMb = mb;
    if (clusterKb) opts.sdClusterKb = clusterKb;
    g_card.cluster = 0;
    cardCount();
}

sim::CardUsage sim::sdUsage(bool recount) {
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    if (recount) cardCount();
    else cardEnsure();
    CardUsage u;
    u.cardBytes = g_card.sizeBytes;
    u.totalBytes = g_card.clusters * g_card.cluster;
    u.usedBytes = std::min(g_card.used, g_card.clusters) * g_card.cluster;
    u.clusterBytes = g_card.cluster;
    return u;
}

namespace fs {

class FileImpl {
public:
    std::string cardPath, host, base;
    FILE* f = nullptr;
    bool dir = false;
    bool append = false;
    uint64_t size = 0;   // as the card's allocation knows it
    std::vector<std::string> entries;
    size_t next = 0;
    ~FileImpl() { if (f) fclose(f); }
};

size_t File::write(const uint8_t* b, size_t n) {
    if (!p_ || !p_->f || !g_present) return 0;
    CHECK_BUS("write", p_->cardPath.c_str());
    cardTime();
    const long at = ftell(p_->f);
    const uint64_t pos = p_->append ? p_->size : (uint64_t)(at < 0 ? 0 : at);
    const size_t fit = cardReserve(p_->size, pos, n);
    if (fit < n) errno = ENOSPC;
    if (!fit) return 0;
    return fwrite(b, 1, fit, p_->f);
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
    const bool truncates = fm[0] == 'w';
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    cardEnsure();
    const uint64_t before = hostSize(impl->host);
    impl->f = fopen(impl->host.c_str(), fm.c_str());
    if (!impl->f) return File();
    if (truncates) cardRelease(before);
    impl->size = truncates ? 0 : before;
    impl->append = fm[0] == 'a';
    return File(impl);
}
bool FS::exists(const char* path) {
    CHECK_BUS("exists", path);
    struct stat st;
    return g_present && stat(hostPath(path).c_str(), &st) == 0;
}
bool FS::remove(const char* path) {
    CHECK_BUS("remove", path);
    if (!g_present) return false;
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    cardEnsure();
    const std::string h = hostPath(path);
    const uint64_t size = hostSize(h);
    if (unlink(h.c_str()) != 0) return false;
    cardRelease(size);
    return true;
}
bool FS::rename(const char* from, const char* to) {
    CHECK_BUS("rename", from);
    if (!g_present) return false;
    struct stat st;
    if (stat(hostPath(to).c_str(), &st) == 0) return false;   // FatFs: FR_EXIST, it never replaces
    return ::rename(hostPath(from).c_str(), hostPath(to).c_str()) == 0;
}
bool FS::mkdir(const char* path) {
    if (!g_present) return false;
    const std::string h = hostPath(path);
    if (isDir(h)) return true;
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    cardEnsure();
    if (!cardFree()) { errno = ENOSPC; return false; }   // a directory is a cluster of its own
    if (::mkdir(h.c_str(), 0755) != 0) return false;
    g_card.used++;
    return true;
}
bool FS::rmdir(const char* path) {
    if (!g_present || ::rmdir(hostPath(path).c_str()) != 0) return false;
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    if (g_card.used) g_card.used--;
    return true;
}

}  // namespace fs

SDFS SD;

bool SDFS::begin(uint8_t, SPIClass&, uint32_t, const char*, uint8_t, bool) {
    initPresent();
    // A card still coming up after power-on fails the init handshake, as a
    // real 32 GB SDHC did for its first ~1.5-2 s (investigations/sd-boot-mount).
    mounted_ = g_present.load() && millis() >= (uint32_t)sim::opts.sdReadyMs;
    if (mounted_) sim::sdUsage(true);   // the card may have changed while out of the slot
    return mounted_;
}
void SDFS::end() { mounted_ = false; }
sdcard_type_t SDFS::cardType() { return g_present ? CARD_SDHC : CARD_NONE; }
uint64_t SDFS::cardSize() { return g_present ? sim::sdUsage().cardBytes : 0; }
uint64_t SDFS::totalBytes() { return g_present ? sim::sdUsage().totalBytes : 0; }
// Recounted on every call: the card may have been written behind the
// firmware's back (a test filling <dir>/sd), and FatFs's f_getfree reads the
// FAT itself.
uint64_t SDFS::usedBytes() { return g_present ? sim::sdUsage(true).usedBytes : 0; }

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

// ---- POSIX calls on the VFS mount ("/sd/...") ------------------------------------
//
// Firmware code that goes around the SD library — opendir("/sd/rides"),
// stat("/sd/..."), fopen("/sd/...") — reaches the card through the ESP-IDF VFS
// mount at /sd. sim_posix.h (force-included into the firmware sources) sends
// those calls here; anything not under /sd goes to the host as is.

namespace {
bool onCard(const char* p) { return p && (!strncmp(p, "/sd/", 4) || !strcmp(p, "/sd")); }

// An fopen()ed card file in write mode: its writes are booked against the
// card like File's (funopen cookie).
struct StdioCard {
    FILE* f;
    uint64_t size;
    bool append;
    std::string path;
};
int stdioRead(void* c, char* b, int n) {
    auto* s = (StdioCard*)c;
    CHECK_BUS("read", s->path.c_str());
    cardTime();
    return (int)fread(b, 1, (size_t)n, s->f);
}
int stdioWrite(void* c, const char* b, int n) {
    auto* s = (StdioCard*)c;
    if (!g_present) { errno = EIO; return -1; }
    CHECK_BUS("write", s->path.c_str());
    cardTime();
    const long at = ftell(s->f);
    const uint64_t pos = s->append ? s->size : (uint64_t)(at < 0 ? 0 : at);
    const size_t fit = cardReserve(s->size, pos, (size_t)n);
    if (!fit) { errno = ENOSPC; return -1; }   // vfs_fat: nothing written and no error -> ENOSPC
    return (int)fwrite(b, 1, fit, s->f);
}
fpos_t stdioSeek(void* c, fpos_t off, int whence) {
    auto* s = (StdioCard*)c;
    if (fseeko(s->f, off, whence) != 0) return -1;
    return ftello(s->f);
}
int stdioClose(void* c) {
    auto* s = (StdioCard*)c;
    int r = fclose(s->f);
    delete s;
    return r;
}
}  // namespace

extern "C" DIR* sim_opendir(const char* path) {
    if (!onCard(path)) return opendir(path);
    if (!g_present) { errno = EIO; return nullptr; }
    CHECK_BUS("opendir", path);
    return opendir(hostPath(path).c_str());
}

extern "C" int sim_stat(const char* path, struct stat* st) {
    if (!onCard(path)) return stat(path, st);
    if (!g_present) { errno = EIO; return -1; }
    CHECK_BUS("stat", path);
    return stat(hostPath(path).c_str(), st);
}

extern "C" FILE* sim_fopen(const char* path, const char* mode) {
    if (!onCard(path)) return fopen(path, mode);
    if (!g_present) { errno = EIO; return nullptr; }
    CHECK_BUS("fopen", path);
    cardTime();
    const std::string h = hostPath(path);
    const bool writes = strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+');
    if (!writes) return fopen(h.c_str(), mode);
    std::lock_guard<std::recursive_mutex> lk(g_card.m);
    cardEnsure();
    const uint64_t before = hostSize(h);
    FILE* f = fopen(h.c_str(), mode);
    if (!f) return nullptr;
    const bool truncates = mode[0] == 'w';
    if (truncates) cardRelease(before);
    auto* s = new StdioCard{f, truncates ? 0 : before, mode[0] == 'a', path};
    return funopen(s, stdioRead, stdioWrite, stdioSeek, stdioClose);
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
