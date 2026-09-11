#if OT_MEMFAULT
#include "memfault_service.h"
#include "diag.h"
#include "sd_bus.h"
#include "ride_recorder.h"
#include "usb_storage.h"
#include "power_mgmt.h"
#include <Arduino.h>
#include <SD.h>
#include <atomic>
#include <mbedtls/sha256.h>
#include "memfault/core/data_export.h"
#include "memfault/core/data_packetizer.h"
#include "memfault/panics/coredump.h"
#include "memfault/panics/platform/coredump.h"

namespace {
std::atomic<int> requested{0};
uint32_t retryAt = 0;
bool consumed = false;
bool flashReadFailed = false;
char id[33] = {};
enum class Output { None, Write, Compare, Serial };
Output output = Output::None;
File* outputFile = nullptr;
bool outputOK = true;
size_t outputBytes = 0;

void emit(const char* text) {
    if (!outputOK) return;
    size_t n = strlen(text);
    if (output == Output::Serial) {
        Serial.println(text);
    } else if (output == Output::Write) {
        outputOK = outputFile->write(reinterpret_cast<const uint8_t*>(text), n) == n &&
                   outputFile->write(uint8_t('\n')) == 1;
    } else if (output == Output::Compare) {
        char actual[MEMFAULT_DATA_EXPORT_BASE64_CHUNK_MAX_LEN];
        outputOK = n < sizeof(actual) && outputFile->read(reinterpret_cast<uint8_t*>(actual), n) == n &&
                   memcmp(actual, text, n) == 0 && outputFile->read() == '\n';
    } else {
        outputOK = false;
    }
    outputBytes += n + 1;
}

// Must be called only by the main task. The upstream packetizer normally
// consumes flash on its final chunk; our wrapper defers that acknowledgement.
bool generate(Output destination, File* file = nullptr) {
    consumed = false; flashReadFailed = false; output = destination; outputFile = file;
    outputOK = true; outputBytes = 0;
    memfault_packetizer_set_active_sources(kMfltDataSourceMask_Coredump);
    uint8_t chunk[MEMFAULT_DATA_EXPORT_CHUNK_MAX_LEN];
    unsigned chunks = 0;
    while (outputOK && !consumed && chunks < 4096) {
        size_t n = sizeof(chunk);
        if (!memfault_packetizer_get_chunk(chunk, &n)) { outputOK = false; break; }
        // Upstream replaces failed reads with a sentinel and continues. Such
        // chunks must never be accepted as a verified archive of this crash.
        if (flashReadFailed) { outputOK = false; break; }
        memfault_data_export_chunk(chunk, n);
        if (++chunks % 16 == 0) vTaskDelay(1);
    }
    bool ok = outputOK && !flashReadFailed && consumed && chunks > 0;
    memfault_packetizer_abort();
    output = Output::None; outputFile = nullptr;
    return ok;
}

bool matches(const char* path) {
    if (!SD.exists(path)) return false;
    File file = SD.open(path, FILE_READ);
    if (!file) return false;
    bool ok = generate(Output::Compare, &file) && file.size() == outputBytes;
    file.close();
    return ok;
}

bool saveSD() {
    char path[80], temp[80];
    snprintf(path, sizeof(path), "/logs/memfault-%s.log", id);
    snprintf(temp, sizeof(temp), "/logs/memfault-%s.tmp", id);
    bool ok = false;
    sdLock();
    if (ride_recorder::sdMounted() && !usb_storage::hostActive()) {
        if (SD.exists(path)) {
            ok = matches(path); // Idempotent after reset before acknowledgement.
        } else {
            SD.mkdir("/logs");
            File file = SD.open(temp, FILE_WRITE);
            if (file) {
                ok = generate(Output::Write, &file);
                file.flush(); file.close();
            }
            ok = ok && matches(temp) && SD.rename(temp, path) && matches(path);
        }
    }
    sdUnlock();
    if (ok) diag::log("memfault: saved and verified %s (%u bytes); acknowledging flash", path, unsigned(outputBytes));
    else diag::log("memfault: SD export failed; crash %s retained in internal flash for retry", id);
    return ok;
}

bool identify() {
    if (id[0]) return true;
    size_t size = 0;
    if (!memfault_service::pending(&size)) return false;
    mbedtls_sha256_context hash;
    mbedtls_sha256_init(&hash);
    bool ok = mbedtls_sha256_starts_ret(&hash, 0) == 0;
    uint8_t buffer[256], digest[32];
    for (size_t offset = 0; ok && offset < size;) {
        size_t n = size - offset; if (n > sizeof(buffer)) n = sizeof(buffer);
        ok = memfault_platform_coredump_storage_read(offset, buffer, n) &&
             mbedtls_sha256_update_ret(&hash, buffer, n) == 0;
        offset += n;
    }
    ok = ok && mbedtls_sha256_finish_ret(&hash, digest) == 0;
    mbedtls_sha256_free(&hash);
    if (ok) for (unsigned i = 0; i < 16; ++i) snprintf(id + i * 2, 3, "%02x", digest[i]);
    return ok;
}

void exportSerial() {
    if (memfault_service::pending()) {
        if (!identify()) { diag::log("memfault: cannot read crash storage"); return; }
        Serial.printf("[memfault] begin id=%s encoding=sdk_data_export\n", id);
        bool ok = generate(Output::Serial);
        Serial.printf("[memfault] end complete=%d bytes=%u (flash retained)\n", ok, unsigned(outputBytes));
    } else if (id[0] && ride_recorder::sdMounted() && !usb_storage::hostActive()) {
        char path[80]; snprintf(path, sizeof(path), "/logs/memfault-%s.log", id);
        sdLock();
        File file = SD.open(path, FILE_READ);
        if (file) {
            Serial.printf("[memfault] begin id=%s encoding=sdk_data_export source=SD\n", id);
            size_t read = 0, expected = file.size();
            uint8_t buffer[256];
            while (read < expected) {
                size_t n = file.read(buffer, sizeof(buffer));
                if (!n || Serial.write(buffer, n) != n) break;
                read += n; vTaskDelay(1);
            }
            file.close();
            Serial.printf("\n[memfault] end complete=%d bytes=%u\n", read == expected, unsigned(read));
        } else diag::log("memfault: saved export unavailable on SD");
        sdUnlock();
    } else diag::log("memfault: no current crash; older exports remain in the phone Diagnostics log list");
}
}

extern "C" void __real_memfault_platform_coredump_storage_clear(void);
extern "C" bool memfault_coredump_read(uint32_t offset, void* buffer, size_t length) {
    bool ok = memfault_platform_coredump_storage_read(offset, buffer, length);
    if (!ok) flashReadFailed = true;
    return ok;
}
extern "C" void __wrap_memfault_platform_coredump_storage_clear(void) {
    consumed = true; // Transport delivery is not durable yet. Do not erase.
}
extern "C" void memfault_data_export_base64_encoded_chunk(const char* chunk) { emit(chunk); }

bool memfault_service::pending(size_t* size) {
    size_t n = 0;
    bool valid = memfault_coredump_has_valid_coredump(&n);
    sMfltCoredumpStorageInfo storage{};
    memfault_platform_coredump_storage_get_info(&storage);
    valid = valid && n > 0 && n <= storage.size;
    if (size) *size = valid ? n : 0;
    return valid;
}
const char* memfault_service::pendingId() { return identify() ? id : "none"; }
void memfault_service::requestStatus(bool exportSerial) { requested = exportSerial ? 2 : 1; }

void memfault_service::tick() {
    int action = requested.exchange(0);
    if (!action && int32_t(millis() - retryAt) < 0) return;
    retryAt = millis() + 30000;
    power_mgmt::busyAcquire();
    size_t size = 0;
    bool saved = pending(&size);
    if (action) diag::log("memfault: pending=%d bytes=%u id=%s SD=%d; offline export, no uploads",
                         saved, unsigned(size), pendingId(), ride_recorder::sdMounted());
    if (action == 2) exportSerial();
    // A failed SD mount never consumes the internal-flash coredump. The SDK
    // preserves the first pending dump if another crash happens before export.
    if (saved && identify() && ride_recorder::sdMounted() && !usb_storage::hostActive() && saveSD()) {
        __real_memfault_platform_coredump_storage_clear();
        if (pending()) diag::log("memfault: flash acknowledgement failed; verified SD export retained, will retry");
    }
    power_mgmt::busyRelease();
}
#endif
