// Host stand-ins for the ESP-IDF calls src/ makes. Power, sleep, GPIO and
// the radio are no-ops; heap is malloc; the clock is sim_clock.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_INVALID_SIZE 0x104
#define ESP_ERR_NOT_FOUND 0x105
#define ESP_ERR_NOT_SUPPORTED 0x106
#define ESP_ERR_TIMEOUT 0x107
#define ESP_ERROR_CHECK(x) (void)(x)
static inline const char* esp_err_to_name(esp_err_t e) { (void)e; return e == ESP_OK ? "ESP_OK" : "ESP_ERR"; }

#define IRAM_ATTR
#define DRAM_ATTR
#define RTC_NOINIT_ATTR
#define RTC_DATA_ATTR
#define RTC_FAST_ATTR
#define RTC_SLOW_ATTR
#define EXT_RAM_BSS_ATTR
#define EXT_RAM_ATTR
#define NOINLINE_ATTR __attribute__((noinline))
#define FORCE_INLINE_ATTR static inline

#define MALLOC_CAP_SPIRAM (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT (1 << 2)
#define MALLOC_CAP_DMA (1 << 3)
#define MALLOC_CAP_DEFAULT (1 << 12)
static inline void* heap_caps_malloc(size_t n, uint32_t caps) { (void)caps; return malloc(n); }
static inline void* heap_caps_calloc(size_t a, size_t b, uint32_t caps) { (void)caps; return calloc(a, b); }
static inline void* heap_caps_realloc(void* p, size_t n, uint32_t caps) { (void)caps; return realloc(p, n); }
static inline void* heap_caps_aligned_alloc(size_t al, size_t n, uint32_t caps) {
    (void)caps; void* p = NULL; if (posix_memalign(&p, al < sizeof(void*) ? sizeof(void*) : al, n)) return NULL; return p; }
static inline void heap_caps_free(void* p) { free(p); }
static inline size_t heap_caps_get_free_size(uint32_t caps) { return (caps & MALLOC_CAP_SPIRAM) ? 6u << 20 : 200u << 10; }
static inline size_t heap_caps_get_largest_free_block(uint32_t caps) { return heap_caps_get_free_size(caps) / 2; }
static inline size_t heap_caps_get_minimum_free_size(uint32_t caps) { return heap_caps_get_free_size(caps); }
static inline size_t esp_get_free_heap_size(void) { return 200u << 10; }
static inline size_t esp_get_minimum_free_heap_size(void) { return 150u << 10; }

uint32_t esp_random(void);
void esp_fill_random(void* buf, size_t len);
int64_t esp_timer_get_time(void);
void esp_restart(void) __attribute__((noreturn));

typedef enum { ESP_RST_UNKNOWN, ESP_RST_POWERON, ESP_RST_EXT, ESP_RST_SW, ESP_RST_PANIC,
               ESP_RST_INT_WDT, ESP_RST_TASK_WDT, ESP_RST_WDT, ESP_RST_DEEPSLEEP,
               ESP_RST_BROWNOUT, ESP_RST_SDIO, ESP_RST_USB, ESP_RST_JTAG } esp_reset_reason_t;
static inline esp_reset_reason_t esp_reset_reason(void) { return ESP_RST_POWERON; }

typedef enum { ESP_SLEEP_WAKEUP_UNDEFINED, ESP_SLEEP_WAKEUP_ALL, ESP_SLEEP_WAKEUP_EXT0,
               ESP_SLEEP_WAKEUP_EXT1, ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_TOUCHPAD,
               ESP_SLEEP_WAKEUP_ULP, ESP_SLEEP_WAKEUP_GPIO, ESP_SLEEP_WAKEUP_UART } esp_sleep_wakeup_cause_t;
typedef int esp_sleep_source_t;
static inline esp_sleep_wakeup_cause_t esp_sleep_get_wakeup_cause(void) { return ESP_SLEEP_WAKEUP_UNDEFINED; }
static inline esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us) { (void)us; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_gpio_wakeup(void) { return ESP_OK; }
static inline esp_err_t esp_sleep_enable_uart_wakeup(int u) { (void)u; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_ext0_wakeup(int p, int l) { (void)p; (void)l; return ESP_OK; }
static inline esp_err_t esp_sleep_enable_ext1_wakeup(uint64_t m, int l) { (void)m; (void)l; return ESP_OK; }
static inline esp_err_t esp_sleep_disable_wakeup_source(int s) { (void)s; return ESP_OK; }
static inline esp_err_t esp_light_sleep_start(void) { return ESP_OK; }
void esp_deep_sleep_start(void) __attribute__((noreturn));
static inline esp_err_t esp_sleep_pd_config(int a, int b) { (void)a; (void)b; return ESP_OK; }
#define ESP_PD_DOMAIN_RTC_PERIPH 0
#define ESP_PD_OPTION_ON 0
#define ESP_PD_OPTION_OFF 1
#define ESP_EXT1_WAKEUP_ANY_LOW 0
#define ESP_EXT1_WAKEUP_ALL_LOW 0
#define ESP_EXT1_WAKEUP_ANY_HIGH 1

typedef void (*esp_timer_cb_t)(void*);
typedef struct esp_timer* esp_timer_handle_t;
typedef enum { ESP_TIMER_TASK, ESP_TIMER_ISR } esp_timer_dispatch_t;
typedef struct { esp_timer_cb_t callback; void* arg; esp_timer_dispatch_t dispatch_method;
                 const char* name; bool skip_unhandled_events; } esp_timer_create_args_t;
esp_err_t esp_timer_create(const esp_timer_create_args_t* a, esp_timer_handle_t* out);
esp_err_t esp_timer_start_once(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t t, uint64_t us);
esp_err_t esp_timer_stop(esp_timer_handle_t t);
esp_err_t esp_timer_delete(esp_timer_handle_t t);

typedef void* esp_pm_lock_handle_t;
typedef enum { ESP_PM_CPU_FREQ_MAX, ESP_PM_APB_FREQ_MAX, ESP_PM_NO_LIGHT_SLEEP } esp_pm_lock_type_t;
typedef struct { int max_freq_mhz; int min_freq_mhz; bool light_sleep_enable; } esp_pm_config_t;
typedef esp_pm_config_t esp_pm_config_esp32s3_t;
static inline esp_err_t esp_pm_lock_create(esp_pm_lock_type_t t, int a, const char* n, esp_pm_lock_handle_t* out) {
    (void)t; (void)a; (void)n; *out = (void*)1; return ESP_OK; }
static inline esp_err_t esp_pm_lock_acquire(esp_pm_lock_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t esp_pm_lock_release(esp_pm_lock_handle_t h) { (void)h; return ESP_OK; }
static inline esp_err_t esp_pm_configure(const void* c) { (void)c; return ESP_OK; }
static inline esp_err_t esp_pm_get_configuration(void* c) { (void)c; return ESP_OK; }
static inline esp_err_t esp_pm_dump_locks(void* f) { (void)f; return ESP_OK; }
typedef bool (*skip_light_sleep_cb_t)(void);
static inline esp_err_t esp_pm_register_skip_light_sleep_callback(skip_light_sleep_cb_t cb) { (void)cb; return ESP_OK; }

// The board's MAC; per instance (--mac), so several sims are distinct mesh nodes.
void sim_efuse_mac(uint8_t* mac);
static inline esp_err_t esp_efuse_mac_get_default(uint8_t* mac) { sim_efuse_mac(mac); return ESP_OK; }
static inline esp_err_t esp_read_mac(uint8_t* mac, int type) { (void)type; return esp_efuse_mac_get_default(mac); }
#define ESP_MAC_WIFI_STA 0
#define ESP_MAC_BT 2
static inline const char* esp_get_idf_version(void) { return "sim"; }
static inline int esp_cpu_get_core_id(void) { return 1; }
static inline void esp_ipc_isr_stall_other_cpu(void) {}
static inline void esp_ipc_isr_release_other_cpu(void) {}
typedef const char* esp_event_base_t;
typedef int (*vprintf_like_t)(const char*, va_list);

#ifdef __cplusplus
}
#endif
