#pragma once

//! @file
//!
//! Copyright (c) Memfault, Inc.
//! See LICENSE for details
//!
//! @brief BLE disconnect session metrics.
//!
//! This module implements a metric "session" (see memfault/metrics/metrics.h)
//! that is opened when a BLE connection is established and closed when it is
//! torn down. Each connection produces its own report, containing:
//!
//! - bt_conn_disconnect_reason_code: the disconnect reason code reported by the
//!   local BLE stack/controller
//!
//! This component has no dependency on any particular BLE stack: platform
//! ports (or applications, on platforms without a dedicated port) are
//! expected to call these functions from their own connection lifecycle
//! code. On Zephyr, this is wired up automatically by
//! memfault_platform_bluetooth_metrics.c when CONFIG_MEMFAULT_METRICS_BLUETOOTH
//! is enabled.
//!
//! Enable with MEMFAULT_METRICS_BLE_SESSION in memfault_platform_config.h.

#include <stdint.h>

#include "memfault/config.h"

#ifdef __cplusplus
extern "C" {
#endif

#if MEMFAULT_METRICS_BLE_SESSION

//! Call when a BLE connection is established.
//! Starts the bt_conn session.
void memfault_metrics_ble_session_connected(void);

//! Call when a BLE connection is torn down. Records the disconnect reason
//! and ends (serializes) the bt_conn session.
//!
//! @param reason_code The disconnect reason code as reported by the local
//! BLE stack/controller (e.g. the HCI status code on Zephyr/most BLE
//! controllers). Recorded as-is; no translation is performed by this API.
void memfault_metrics_ble_session_disconnected(uint8_t reason_code);

#endif  // MEMFAULT_METRICS_BLE_SESSION

#ifdef __cplusplus
}
#endif
