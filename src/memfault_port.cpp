#if OT_MEMFAULT
#include "memfault_service.h"
#include "diag.h"
#include "config.h"
#include <Arduino.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <soc/soc.h>
#include <stdarg.h>
#include "memfault/core/log.h"
#include "memfault/core/event_storage.h"
#include "memfault/core/platform/core.h"
#include "memfault/core/platform/device_info.h"
#include "memfault/core/reboot_tracking.h"
#include "memfault/panics/arch/xtensa/xtensa.h"
#include "memfault/panics/coredump.h"

namespace {
RTC_NOINIT_ATTR uint8_t rebootState[MEMFAULT_REBOOT_TRACKING_REGION_SIZE];
uint8_t logStorage[2048];
uint8_t bootEventStorage[512];
char deviceSerial[16] = "esp32s3";
bool ready = false;
portMUX_TYPE logMux = portMUX_INITIALIZER_UNLOCKED;

eMemfaultRebootReason rebootReason(int reason) {
    switch (reason) {
        case ESP_RST_POWERON: return kMfltRebootReason_PowerOnReset;
        case ESP_RST_SW: return kMfltRebootReason_SoftwareReset;
        case ESP_RST_INT_WDT: return kMfltRebootReason_SoftwareWatchdog;
        case ESP_RST_TASK_WDT: return kMfltRebootReason_TaskWatchdog;
        case ESP_RST_WDT: return kMfltRebootReason_HardwareWatchdog;
        case ESP_RST_DEEPSLEEP: return kMfltRebootReason_DeepSleep;
        case ESP_RST_BROWNOUT: return kMfltRebootReason_BrownOutReset;
        case ESP_RST_PANIC: return kMfltRebootReason_UnknownError;
        default: return kMfltRebootReason_Unknown;
    }
}
}

extern "C" {
// Dual-core SDK collection uses the IDF 5 name; Arduino 2.x/IDF 4.4 exposes
// the same CPU ID through FreeRTOS. No changes to precompiled framework libs.
int IRAM_ATTR esp_cpu_get_core_id(void) { return xPortGetCoreID(); }
void memfault_lock(void) { portENTER_CRITICAL(&logMux); }
void memfault_unlock(void) { portEXIT_CRITICAL(&logMux); }
bool memfault_arch_is_inside_isr(void) { return xPortInIsrContext(); }
uint64_t memfault_platform_get_time_since_boot_ms(void) { return esp_timer_get_time() / 1000; }
void memfault_platform_halt_if_debugging(void) {}
MEMFAULT_NORETURN void memfault_sdk_assert_func_noreturn(void) { abort(); }
MEMFAULT_NORETURN void memfault_platform_reboot(void) { esp_restart(); abort(); }

// SDK diagnostic messages may occur before Arduino/CDC starts. Keep them in
// Memfault's RAM logger; never route them through UART0 (the GPS TX pin).
void memfault_platform_log(eMemfaultPlatformLogLevel level, const char* fmt, ...) {
    if (!ready) return;
    uint32_t ps;
    asm volatile("rsr %0, ps" : "=a"(ps));
    if ((ps & 0xf) || xPortInIsrContext()) return; // Never lock/format in a panic.
    char line[160];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap); va_end(ap);
    memfault_log_save_preformatted(level, line, strlen(line));
}
void memfault_platform_log_raw(const char* fmt, ...) { (void)fmt; }

void memfault_platform_get_device_info(sMemfaultDeviceInfo* info) {
    *info = {deviceSerial, "opentrailpaper", FIRMWARE_VERSION,
             "lilygo-t5s3-gps"};
}

// Arduino ships precompiled FreeRTOS without Memfault task-registry hooks.
// Capture both stopped CPU stacks and their current TCBs without traversing
// potentially corrupted task lists or taking any RTOS locks in the panic.
extern void* volatile pxCurrentTCB[2];
const sMfltCoredumpRegion* memfault_platform_coredump_get_regions(
    const sCoredumpCrashInfo* info, size_t* count) {
    static sMfltCoredumpRegion regions[7];
    size_t n = 0;
    auto add = [&](void* address, size_t length) {
        uintptr_t p = reinterpret_cast<uintptr_t>(address);
        if (p < SOC_DRAM_LOW || p >= SOC_DRAM_HIGH) return;
        size_t available = SOC_DRAM_HIGH - p;
        if (length > available) length = available;
        if (length) regions[n++] = MEMFAULT_COREDUMP_MEMORY_REGION_INIT(address, length);
    };
    add(info->stack_address, 8192);
    if (info->exception_reg_state) {
        const auto* regs = static_cast<const sMfltRegState*>(info->exception_reg_state);
        for (unsigned core = 0; core < 2; ++core) {
            uintptr_t sp = regs[core].a[1];
            if (sp >= 64 && sp - 64 != reinterpret_cast<uintptr_t>(info->stack_address))
                add(reinterpret_cast<void*>(sp - 64), 8192);
        }
    }
    add((void*)pxCurrentTCB, sizeof(pxCurrentTCB));
    for (unsigned core = 0; core < 2; ++core) add(pxCurrentTCB[core], 512);
    extern uint8_t _bss_start[], _bss_end[];
    size_t bss = _bss_end - _bss_start;
    add(_bss_start, bss < 32768 ? bss : 32768);
    *count = n;
    return regions;
}
}

void memfault_service::begin(int resetReason) {
    uint8_t mac[6];
    if (esp_efuse_mac_get_default(mac) == ESP_OK)
        snprintf(deviceSerial, sizeof(deviceSerial), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    sResetBootupInfo bootInfo = {uint32_t(resetReason), rebootReason(resetReason)};
    memfault_reboot_tracking_boot(rebootState, &bootInfo);
    ready = memfault_log_boot(logStorage, sizeof(logStorage));
    sMfltRebootReason stored{};
    int reasonResult = memfault_reboot_tracking_get_reboot_reason(&stored);
    // The public SDK collector consumes the previous RTC reset record so the
    // next panic can record its own reason. No heartbeat or uploader is started.
    const auto* events = memfault_events_storage_boot(bootEventStorage, sizeof(bootEventStorage));
    int collectResult = memfault_reboot_tracking_collect_reset_info(events);
    size_t size = 0;
    bool saved = pending(&size);
    diag::log("memfault: SDK ready=%d reboot=0x%x reason_rc=%d collect_rc=%d core_pending=%d bytes=%u id=%s; offline only",
              ready, unsigned(stored.prior_stored_reason), reasonResult, collectResult,
              saved, unsigned(size), pendingId());
}

void memfault_service::recordLine(const char* line, size_t length) {
    if (ready) memfault_log_save_preformatted(kMemfaultPlatformLogLevel_Info, line, length);
}
#endif
