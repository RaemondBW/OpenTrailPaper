#pragma once
#include "../esp_sim.h"
typedef int gpio_num_t;
#define GPIO_NUM_NC -1
#define GPIO_NUM_0 0
#define GPIO_NUM_1 1
#define GPIO_NUM_2 2
#define GPIO_NUM_3 3
#define GPIO_NUM_4 4
#define GPIO_NUM_5 5
#define GPIO_NUM_6 6
#define GPIO_NUM_7 7
#define GPIO_NUM_8 8
#define GPIO_NUM_9 9
#define GPIO_NUM_10 10
#define GPIO_NUM_11 11
#define GPIO_NUM_12 12
#define GPIO_NUM_13 13
#define GPIO_NUM_14 14
#define GPIO_NUM_15 15
#define GPIO_NUM_16 16
#define GPIO_NUM_17 17
#define GPIO_NUM_18 18
#define GPIO_NUM_19 19
#define GPIO_NUM_20 20
#define GPIO_NUM_21 21
#define GPIO_NUM_38 38
#define GPIO_NUM_39 39
#define GPIO_NUM_40 40
#define GPIO_NUM_41 41
#define GPIO_NUM_42 42
#define GPIO_NUM_43 43
#define GPIO_NUM_44 44
#define GPIO_NUM_45 45
#define GPIO_NUM_46 46
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
typedef enum { GPIO_MODE_DISABLE = 0, GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2,
               GPIO_MODE_OUTPUT_OD = 6, GPIO_MODE_INPUT_OUTPUT_OD = 7, GPIO_MODE_INPUT_OUTPUT = 3 } gpio_mode_t;
typedef enum { GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE, GPIO_INTR_NEGEDGE, GPIO_INTR_ANYEDGE,
               GPIO_INTR_LOW_LEVEL, GPIO_INTR_HIGH_LEVEL } gpio_int_type_t;
typedef enum { GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE } gpio_pullup_t;
typedef enum { GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE } gpio_pulldown_t;
typedef enum { GPIO_DRIVE_CAP_0, GPIO_DRIVE_CAP_1, GPIO_DRIVE_CAP_2, GPIO_DRIVE_CAP_3 } gpio_drive_cap_t;
typedef struct { uint64_t pin_bit_mask; gpio_mode_t mode; gpio_pullup_t pull_up_en;
                 gpio_pulldown_t pull_down_en; gpio_int_type_t intr_type; } gpio_config_t;
#ifdef __cplusplus
extern "C" {
#endif
static inline esp_err_t gpio_config(const gpio_config_t* c) { (void)c; return ESP_OK; }
static inline esp_err_t gpio_reset_pin(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t p, gpio_mode_t m) { (void)p; (void)m; return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t p, uint32_t l) { (void)p; (void)l; return ESP_OK; }
int gpio_get_level(gpio_num_t p);
static inline esp_err_t gpio_pullup_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_pullup_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_pulldown_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_pulldown_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_hold_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_hold_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline void gpio_deep_sleep_hold_en(void) {}
static inline void gpio_deep_sleep_hold_dis(void) {}
static inline esp_err_t gpio_sleep_sel_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_sleep_sel_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_wakeup_enable(gpio_num_t p, gpio_int_type_t t) { (void)p; (void)t; return ESP_OK; }
static inline esp_err_t gpio_wakeup_disable(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t gpio_set_drive_capability(gpio_num_t p, gpio_drive_cap_t c) { (void)p; (void)c; return ESP_OK; }
static inline esp_err_t gpio_set_intr_type(gpio_num_t p, gpio_int_type_t t) { (void)p; (void)t; return ESP_OK; }
static inline esp_err_t gpio_install_isr_service(int f) { (void)f; return ESP_OK; }
static inline esp_err_t gpio_isr_handler_add(gpio_num_t p, void (*h)(void*), void* a) { (void)p; (void)h; (void)a; return ESP_OK; }
static inline esp_err_t gpio_sleep_set_direction(gpio_num_t p, gpio_mode_t m) { (void)p; (void)m; return ESP_OK; }
static inline esp_err_t gpio_sleep_set_pull_mode(gpio_num_t p, int m) { (void)p; (void)m; return ESP_OK; }
static inline esp_err_t rtc_gpio_pullup_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_pullup_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_pulldown_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_en(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_hold_dis(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_init(gpio_num_t p) { (void)p; return ESP_OK; }
static inline esp_err_t rtc_gpio_deinit(gpio_num_t p) { (void)p; return ESP_OK; }
static inline bool rtc_gpio_is_valid_gpio(gpio_num_t p) { (void)p; return true; }
#ifdef __cplusplus
}
#endif
