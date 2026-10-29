#pragma once
#include "esp_sim.h"
typedef struct { uint32_t magic_word; uint32_t secure_version; uint32_t reserv1[2];
                 char version[32]; char project_name[32]; char time[16]; char date[16];
                 char idf_ver[32]; uint8_t app_elf_sha256[32]; } esp_app_desc_t;
typedef struct { int type; int subtype; uint32_t address; uint32_t size; char label[17]; } esp_partition_t;
#ifdef __cplusplus
extern "C" {
#endif
const esp_app_desc_t* esp_ota_get_app_description(void);
static inline const esp_partition_t* esp_ota_get_running_partition(void) { static esp_partition_t p = {0, 0x10, 0x10000, 0x640000, "app0"}; return &p; }
static inline const esp_partition_t* esp_ota_get_boot_partition(void) { return esp_ota_get_running_partition(); }
static inline esp_err_t esp_ota_mark_app_valid_cancel_rollback(void) { return ESP_OK; }
#ifdef __cplusplus
}
#endif
