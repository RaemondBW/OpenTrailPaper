#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Sim: tells connected centrals the GATT table changed (simble SERVICE_CHANGED).
void ble_svc_gatt_changed(uint16_t start_handle, uint16_t end_handle);
#ifdef __cplusplus
}
#endif
