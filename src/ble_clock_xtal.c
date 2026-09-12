// Project-local rebuild of the matching IDF controller adapter. The original
// libbt.a member must NOT appear in the link map for the XTAL environment.
#ifdef PM_BLE_XTAL
#include "sdkconfig.h"
#include "esp_idf_version.h"
#include <stdbool.h>
#include <stdatomic.h>
#if ESP_IDF_VERSION != ESP_IDF_VERSION_VAL(4, 4, 6) || !CONFIG_IDF_TARGET_ESP32S3
#error "BLE XTAL adapter is pinned to ESP32-S3 / ESP-IDF 4.4.6"
#endif
#if !CONFIG_PM_ENABLE || !CONFIG_FREERTOS_USE_TICKLESS_IDLE || !CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1
#error "BLE XTAL experiment requires the PM-enabled framework"
#endif

// These options affect the controller adapter below. SDK headers/archives in
// the installed framework stay untouched. The caller's config is corrected at
// our public init entry point too, because its precompiled default can be RC.
#undef CONFIG_BT_CTRL_LPCLK_SEL_RTC_SLOW
#undef CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL
#undef CONFIG_BT_CTRL_SLEEP_CLOCK_EFF
#define CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL 1
#define CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP 1
#define CONFIG_BT_CTRL_SLEEP_CLOCK_EFF 1
#define esp_bt_controller_init board_bt_controller_init
#include "../tools/ble_clock_vendor/bt.c"
#undef esp_bt_controller_init

static atomic_bool xtal_ready = false;
esp_err_t esp_bt_controller_init(esp_bt_controller_config_t *cfg) {
    if (!cfg) return ESP_ERR_INVALID_ARG;
    cfg->sleep_mode = ESP_BT_SLEEP_MODE_1;
    cfg->sleep_clock = ESP_BT_SLEEP_CLOCK_MAIN_XTAL;
    esp_err_t err = board_bt_controller_init(cfg);
    atomic_store(&xtal_ready, err == ESP_OK);
    return err;
}

bool board_ble_xtal_clock_ready(void) {
    return atomic_load(&xtal_ready) && esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_ENABLED;
}
#endif
