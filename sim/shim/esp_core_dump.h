#pragma once
// No core dump partition in the sim: CONFIG_ESP_COREDUMP_* stay undefined, so
// crash_report.cpp writes "core_summary=not enabled" and keeps the rest.
#include "esp_sim.h"
#ifdef __cplusplus
extern "C" {
#endif
static inline esp_err_t esp_core_dump_image_erase(void) { return ESP_OK; }
#ifdef __cplusplus
}
#endif
