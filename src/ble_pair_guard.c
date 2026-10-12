#include "ble_pair_guard.h"

#include "nimble/nimble/host/src/ble_hs_priv.h"

// One slot per guarded link: the callback NimBLE-Arduino installed, which
// every event but a refused repeat pairing is forwarded to. The server only
// ever holds one central at a time (advertising stops while it is up), but a
// link being torn down can overlap the next one's connect.
typedef struct {
    uint16_t conn;
    ble_gap_event_fn* cb;
    void* arg;
} guard_slot;

static guard_slot s_slots[3] = {
    {BLE_HS_CONN_HANDLE_NONE, NULL, NULL},
    {BLE_HS_CONN_HANDLE_NONE, NULL, NULL},
    {BLE_HS_CONN_HANDLE_NONE, NULL, NULL},
};
static ble_pair_guard_refuse_fn* s_refuse = NULL;

void ble_pair_guard_harden(void) {
    // Secure Connections Only: the host refuses a legacy pairing request from
    // a phone, and every attribute that asks for security needs an
    // authenticated, encrypted link with a 16-byte key. Global, so it also
    // reaches the sensor (central) side — there only the key-size check
    // applies (ble_sm.c's pair-response path), not the SC flag.
    ble_hs_cfg.sm_sc_only = 1;
    // CCCDs (0x2902) are created by the host, not by NimBLE-Arduino, and are
    // open by default: an unencrypted central could subscribe. Same bar as the
    // characteristics they belong to.
    ble_gatts_set_clt_cfg_perm_flags(BLE_ATT_F_READ | BLE_ATT_F_WRITE |
                                     BLE_ATT_F_READ_ENC | BLE_ATT_F_WRITE_ENC |
                                     BLE_ATT_F_READ_AUTHEN |
                                     BLE_ATT_F_WRITE_AUTHEN);
}

void ble_pair_guard_set_refuse(ble_pair_guard_refuse_fn* fn) { s_refuse = fn; }

static int guard_cb(struct ble_gap_event* event, void* arg) {
    guard_slot* slot = (guard_slot*)arg;
    ble_gap_event_fn* cb = slot->cb;
    void* cbArg = slot->arg;
    if (event->type == BLE_GAP_EVENT_REPEAT_PAIRING && s_refuse &&
        s_refuse(event->repeat_pairing.conn_handle)) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    if (event->type == BLE_GAP_EVENT_DISCONNECT) {
        slot->conn = BLE_HS_CONN_HANDLE_NONE;   // free before forwarding
        slot->cb = NULL;
        slot->arg = NULL;
    }
    return cb ? cb(event, cbArg) : 0;
}

int ble_pair_guard_install(uint16_t conn_handle) {
    guard_slot* slot = NULL;
    for (size_t i = 0; i < sizeof(s_slots) / sizeof(s_slots[0]); ++i) {
        // A handle the controller reused takes its old slot back.
        if (s_slots[i].conn == conn_handle) { slot = &s_slots[i]; break; }
        if (!slot && s_slots[i].conn == BLE_HS_CONN_HANDLE_NONE) slot = &s_slots[i];
    }
    if (!slot) return BLE_HS_ENOMEM;

    int rc = 0;
    ble_hs_lock();
    struct ble_hs_conn* conn = ble_hs_conn_find(conn_handle);
    if (!conn) {
        rc = BLE_HS_ENOTCONN;
    } else if (conn->bhc_cb != guard_cb) {
        slot->conn = conn_handle;
        slot->cb = conn->bhc_cb;
        slot->arg = conn->bhc_cb_arg;
        conn->bhc_cb = guard_cb;
        conn->bhc_cb_arg = slot;
    }
    ble_hs_unlock();
    return rc;
}
