// BLE sensor simulator for bench-testing the bike computer.
//
// One ESP32-C6 pretends to be two separate peripherals by running two
// extended-advertising instances, each with its own random static address and
// name:
//
//   instance 0  "Magene H64"     Heart Rate (0x180D) + Battery (0x180F)
//   instance 1  "ASSIOMA12345"   Cycling Power (0x1818) + Battery (0x180F)
//
// NimBLE has a single GATT table, so both "devices" technically expose every
// service. The bike computer only looks up the service it paired for plus the
// Device Information Service, and the DIS strings are answered per-connection
// (the connection's advertising instance decides whether it is Magene or
// Favero), so from its point of view they are two vendors' sensors.
//
// Console (USB serial JTAG, 115200): type `help`. `auto` runs a scripted ride;
// `hr 150`, `pwr 250`, `cad 90` pin values; `off pwr` / `on pwr` simulate a
// sensor dropping out and coming back.

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs_flash.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "sim";

enum { DEV_HR = 0, DEV_PWR = 1, DEV_COUNT };

typedef struct {
    const char *label;   // console name
    const char *adv_name;
    const char *mfr;
    const char *model;
    const char *serial;
    ble_addr_t addr;
    uint16_t svc_uuid;   // primary service advertised
    bool enabled;        // advertising / accepting connections
    uint16_t conn;       // BLE_HS_CONN_HANDLE_NONE when idle
    bool sub_meas;       // central subscribed to the measurement characteristic
    bool sub_batt;
    uint8_t batt_pct;
} device_t;

static device_t devs[DEV_COUNT] = {
    [DEV_HR] = {
        .label = "hr", .adv_name = "Magene H64",
        .mfr = "Magene", .model = "H64", .serial = "H64-SIM-0001",
        .addr = {.type = BLE_ADDR_RANDOM, .val = {0x01, 0x00, 0x00, 0x11, 0x5E, 0xC6}},
        .svc_uuid = 0x180D, .enabled = true, .conn = BLE_HS_CONN_HANDLE_NONE, .batt_pct = 87,
    },
    [DEV_PWR] = {
        .label = "pwr", .adv_name = "ASSIOMA12345",
        .mfr = "Favero Electronics", .model = "Assioma DUO", .serial = "12345",
        .addr = {.type = BLE_ADDR_RANDOM, .val = {0x02, 0x00, 0x00, 0x11, 0x5E, 0xC6}},
        .svc_uuid = 0x1818, .enabled = true, .conn = BLE_HS_CONN_HANDLE_NONE, .batt_pct = 64,
    },
};

// ---- simulated physiology -------------------------------------------------

static struct {
    bool auto_mode;
    uint32_t t;          // seconds since boot
    int hr_bpm;
    bool contact;
    int watts;
    int cad_rpm;
    int balance_pct;     // left leg share
    bool jitter;         // manual mode: flutter around the set values like a real sensor
    // what actually goes out on the air this second
    int out_hr, out_watts, out_cad, out_bal;
    // crank state as the Cycling Power spec wants it
    uint16_t crank_revs;
    uint16_t crank_evt;  // 1/1024 s units, wraps
    double rev_accum;
} sim = {.auto_mode = true, .hr_bpm = 140, .contact = true, .watts = 200,
         .cad_rpm = 88, .balance_pct = 50, .jitter = true};

// Uniform noise in [-n, n]; 0 when the base value is 0 (a stopped crank stays stopped).
static int wobble(int base, int n) { return base == 0 ? 0 : base + (rand() % (2 * n + 1)) - n; }

static void sim_step(void) {
    sim.t++;
    if (sim.auto_mode) {
        double t = sim.t;
        // A gentle interval session: 3 min "on" / 3 min "off" for power, HR
        // lagging behind, cadence with some wobble.
        double on = (fmod(t, 360.0) < 180.0) ? 1.0 : 0.0;
        sim.watts = (int)(160 + on * 90 + 20 * sin(t / 7.0) + (rand() % 11 - 5));
        sim.hr_bpm = (int)(128 + on * 22 + 6 * sin(t / 23.0) + (rand() % 3 - 1));
        sim.cad_rpm = (int)(86 + on * 4 + 4 * sin(t / 11.0));
        sim.balance_pct = 50 + (int)(2 * sin(t / 5.0));
    }
    if (sim.auto_mode || !sim.jitter) {
        sim.out_hr = sim.hr_bpm; sim.out_watts = sim.watts;
        sim.out_cad = sim.cad_rpm; sim.out_bal = sim.balance_pct;
    } else {
        // Slow drift + per-sample noise: HR a couple of bpm, power ~5 %, cadence a few rpm.
        double t = sim.t;
        sim.out_hr = wobble((int)(sim.hr_bpm + 2 * sin(t / 9.0)), 1);
        sim.out_watts = wobble((int)(sim.watts * (1 + 0.03 * sin(t / 4.0))), sim.watts / 25 + 1);
        sim.out_cad = wobble((int)(sim.cad_rpm + 2 * sin(t / 6.0)), 1);
        sim.out_bal = wobble(sim.balance_pct, 1);
    }
    if (sim.out_hr < 0) sim.out_hr = 0;
    if (sim.out_watts < 0) sim.out_watts = 0;
    if (sim.out_cad < 0) sim.out_cad = 0;
    // Advance the crank. `crank_evt` is the time of the last full revolution.
    uint32_t now_1024 = (uint32_t)(esp_timer_get_time() * 1024 / 1000000);
    if (sim.out_cad > 0) {
        sim.rev_accum += sim.out_cad / 60.0;
        if (sim.rev_accum >= 1.0) {
            int whole = (int)sim.rev_accum;
            sim.rev_accum -= whole;
            sim.crank_revs += whole;
            double rev_period_s = 60.0 / sim.out_cad;
            sim.crank_evt = (uint16_t)(now_1024 - (uint32_t)(sim.rev_accum * rev_period_s * 1024));
        }
    } else {
        sim.rev_accum = 0;
    }
}

// ---- GATT -------------------------------------------------------------------

enum {
    CHR_HR_MEAS, CHR_HR_LOC, CHR_BATT, CHR_DIS_MFR, CHR_DIS_MODEL, CHR_DIS_SERIAL,
    CHR_CP_MEAS, CHR_CP_FEAT, CHR_CP_LOC,
};

static uint16_t h_hr_meas, h_batt, h_cp_meas;

static device_t *dev_for_conn(uint16_t conn) {
    for (int i = 0; i < DEV_COUNT; ++i)
        if (devs[i].conn == conn) return &devs[i];
    return NULL;
}

static int gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg) {
    if (ctxt->op != BLE_GATT_ACCESS_OP_READ_CHR) return BLE_ATT_ERR_UNLIKELY;
    device_t *d = dev_for_conn(conn_handle);
    if (!d) d = &devs[DEV_HR];
    const char *s = NULL;
    uint8_t b[4];
    int n = 0;
    switch ((int)(intptr_t)arg) {
        case CHR_DIS_MFR:    s = d->mfr; break;
        case CHR_DIS_MODEL:  s = d->model; break;
        case CHR_DIS_SERIAL: s = d->serial; break;
        case CHR_BATT:       b[0] = d->batt_pct; n = 1; break;
        case CHR_HR_LOC:     b[0] = 1; n = 1; break;          // chest
        case CHR_CP_LOC:     b[0] = 7; n = 1; break;          // left pedal
        case CHR_CP_FEAT: {
            // pedal power balance | crank revolution data
            uint32_t f = 0x00000001 | 0x00000008;
            memcpy(b, &f, 4); n = 4; break;
        }
        case CHR_HR_MEAS: case CHR_CP_MEAS: return BLE_ATT_ERR_READ_NOT_PERMITTED;
        default: return BLE_ATT_ERR_UNLIKELY;
    }
    int rc = s ? os_mbuf_append(ctxt->om, s, strlen(s)) : os_mbuf_append(ctxt->om, b, n);
    return rc == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
}

#define U16(x) BLE_UUID16_DECLARE(x)
#define ARG(x) ((void *)(intptr_t)(x))

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {   .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180D),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = U16(0x2A37), .access_cb = gatt_access, .arg = ARG(CHR_HR_MEAS),
              .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_hr_meas },
            { .uuid = U16(0x2A38), .access_cb = gatt_access, .arg = ARG(CHR_HR_LOC),
              .flags = BLE_GATT_CHR_F_READ },
            { 0 } } },
    {   .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x1818),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = U16(0x2A63), .access_cb = gatt_access, .arg = ARG(CHR_CP_MEAS),
              .flags = BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_cp_meas },
            { .uuid = U16(0x2A65), .access_cb = gatt_access, .arg = ARG(CHR_CP_FEAT),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A5D), .access_cb = gatt_access, .arg = ARG(CHR_CP_LOC),
              .flags = BLE_GATT_CHR_F_READ },
            { 0 } } },
    {   .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180F),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = U16(0x2A19), .access_cb = gatt_access, .arg = ARG(CHR_BATT),
              .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &h_batt },
            { 0 } } },
    {   .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = U16(0x180A),
        .characteristics = (struct ble_gatt_chr_def[]) {
            { .uuid = U16(0x2A29), .access_cb = gatt_access, .arg = ARG(CHR_DIS_MFR),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A24), .access_cb = gatt_access, .arg = ARG(CHR_DIS_MODEL),
              .flags = BLE_GATT_CHR_F_READ },
            { .uuid = U16(0x2A25), .access_cb = gatt_access, .arg = ARG(CHR_DIS_SERIAL),
              .flags = BLE_GATT_CHR_F_READ },
            { 0 } } },
    { 0 },
};

static void notify(device_t *d, uint16_t attr, const uint8_t *buf, int len) {
    if (d->conn == BLE_HS_CONN_HANDLE_NONE) return;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(buf, len);
    if (!om) return;
    int rc = ble_gatts_notify_custom(d->conn, attr, om);
    if (rc) ESP_LOGW(TAG, "%s notify rc=%d", d->label, rc);
}

static void tick(void *arg) {
    sim_step();

    // Heart Rate Measurement: flags | bpm u8 | RR u16 (1/1024 s)
    {
        device_t *d = &devs[DEV_HR];
        uint8_t flags = 0x04 | (sim.contact ? 0x02 : 0) | 0x10;
        uint16_t rr = sim.out_hr > 0 ? (uint16_t)(60.0 * 1024 / sim.out_hr) : 0;
        uint8_t p[4] = { flags, (uint8_t)sim.out_hr, rr & 0xFF, rr >> 8 };
        if (d->sub_meas) notify(d, h_hr_meas, p, sizeof p);
    }
    // Cycling Power Measurement: flags u16 | watts s16 | balance u8 | crank revs u16 | crank evt u16
    {
        device_t *d = &devs[DEV_PWR];
        uint16_t flags = 0x0001 | 0x0002 | 0x0020;   // balance present, left ref, crank data
        int16_t w = (int16_t)sim.out_watts;
        uint8_t p[9] = { flags & 0xFF, flags >> 8, w & 0xFF, (w >> 8) & 0xFF,
                         (uint8_t)(sim.out_bal * 2),
                         sim.crank_revs & 0xFF, sim.crank_revs >> 8,
                         sim.crank_evt & 0xFF, sim.crank_evt >> 8 };
        if (d->sub_meas) notify(d, h_cp_meas, p, sizeof p);
    }
    if (sim.t % 60 == 0) {
        for (int i = 0; i < DEV_COUNT; ++i)
            if (devs[i].sub_batt) notify(&devs[i], h_batt, &devs[i].batt_pct, 1);
    }
}

// ---- advertising --------------------------------------------------------------

static int gap_event(struct ble_gap_event *event, void *arg);

static void adv_start(device_t *d) {
    uint8_t inst = (uint8_t)(d - devs);
    if (!d->enabled || ble_gap_ext_adv_active(inst)) return;

    struct ble_gap_ext_adv_params p = {0};
    p.connectable = 1;
    p.scannable = 1;
    p.legacy_pdu = 1;                 // plain ADV_IND so any scanner sees it
    p.own_addr_type = BLE_OWN_ADDR_RANDOM;
    p.primary_phy = BLE_HCI_LE_PHY_1M;
    p.secondary_phy = BLE_HCI_LE_PHY_1M;
    p.sid = inst;
    p.itvl_min = p.itvl_max = BLE_GAP_ADV_ITVL_MS(250);
    p.tx_power = 127;
    int rc = ble_gap_ext_adv_configure(inst, &p, NULL, gap_event, d);
    if (rc) { ESP_LOGE(TAG, "%s adv configure rc=%d", d->label, rc); return; }
    rc = ble_gap_ext_adv_set_addr(inst, &d->addr);
    if (rc) { ESP_LOGE(TAG, "%s adv set addr rc=%d", d->label, rc); return; }

    ble_uuid16_t uuids[2] = { BLE_UUID16_INIT(d->svc_uuid), BLE_UUID16_INIT(0x180F) };
    struct ble_hs_adv_fields f = {0};
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids16 = uuids;
    f.num_uuids16 = 2;
    f.uuids16_is_complete = 1;
    f.name = (uint8_t *)d->adv_name;
    f.name_len = strlen(d->adv_name);
    f.name_is_complete = 1;
    struct os_mbuf *om = os_msys_get_pkthdr(BLE_HCI_MAX_ADV_DATA_LEN, 0);
    rc = ble_hs_adv_set_fields_mbuf(&f, om);
    if (rc == 0) rc = ble_gap_ext_adv_set_data(inst, om);
    else os_mbuf_free_chain(om);
    if (rc) { ESP_LOGE(TAG, "%s adv data rc=%d", d->label, rc); return; }

    rc = ble_gap_ext_adv_start(inst, 0, 0);
    if (rc) ESP_LOGE(TAG, "%s adv start rc=%d", d->label, rc);
    else ESP_LOGI(TAG, "%s advertising as \"%s\"", d->label, d->adv_name);
}

static void adv_stop(device_t *d) {
    uint8_t inst = (uint8_t)(d - devs);
    if (ble_gap_ext_adv_active(inst)) ble_gap_ext_adv_stop(inst);
}

static int gap_event(struct ble_gap_event *event, void *arg) {
    device_t *d = arg;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) {
                d->conn = event->connect.conn_handle;
                d->sub_meas = d->sub_batt = false;
                ESP_LOGI(TAG, "%s connected (handle %u)", d->label, d->conn);
            } else {
                ESP_LOGW(TAG, "%s connect failed %d", d->label, event->connect.status);
                adv_start(d);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "%s disconnected (reason %d)", d->label, event->disconnect.reason);
            d->conn = BLE_HS_CONN_HANDLE_NONE;
            d->sub_meas = d->sub_batt = false;
            adv_start(d);
            break;
        case BLE_GAP_EVENT_SUBSCRIBE: {
            uint16_t h = event->subscribe.attr_handle;
            bool on = event->subscribe.cur_notify;
            if (h == h_hr_meas || h == h_cp_meas) d->sub_meas = on;
            else if (h == h_batt) d->sub_batt = on;
            ESP_LOGI(TAG, "%s subscribe handle %u -> %s", d->label, h, on ? "on" : "off");
            break;
        }
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "%s adv complete reason=%d", d->label, event->adv_complete.reason);
            if (event->adv_complete.reason != 0) adv_start(d);
            break;
        default:
            break;
    }
    return 0;
}

// Which device(s) this board plays. A BLE controller accepts only ONE link per
// peer address, so a single central (the bike computer) can hold a connection
// to just one of the two instances at a time; to test both sensors together
// run two boards, `role hr` on one and `role pwr` on the other.
static uint8_t role_mask = 0x03;   // bit0 = hr, bit1 = pwr

static void role_load(void) {
    nvs_handle_t h;
    if (nvs_open("sim", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "role", &role_mask);
        nvs_close(h);
    }
    for (int i = 0; i < DEV_COUNT; ++i) devs[i].enabled = (role_mask >> i) & 1;
}

static void role_save(void) {
    nvs_handle_t h;
    if (nvs_open("sim", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "role", role_mask);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void on_sync(void) {
    for (int i = 0; i < DEV_COUNT; ++i) adv_start(&devs[i]);
}

static void on_reset(int reason) { ESP_LOGW(TAG, "host reset %d", reason); }

static void host_task(void *p) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// ---- console ------------------------------------------------------------------

static device_t *dev_by_label(const char *s) {
    for (int i = 0; i < DEV_COUNT; ++i)
        if (strcmp(devs[i].label, s) == 0) return &devs[i];
    return NULL;
}

static int cmd_set(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s <value>\n", argv[0]); return 1; }
    int v = atoi(argv[1]);
    sim.auto_mode = false;
    if (!strcmp(argv[0], "hr")) sim.hr_bpm = v;
    else if (!strcmp(argv[0], "pwr")) sim.watts = v;
    else if (!strcmp(argv[0], "cad")) sim.cad_rpm = v;
    else if (!strcmp(argv[0], "bal")) sim.balance_pct = v;
    printf("ok (auto off)\n");
    return 0;
}

static int cmd_contact(int argc, char **argv) {
    if (argc < 2) { printf("usage: contact on|off\n"); return 1; }
    sim.contact = !strcmp(argv[1], "on");
    return 0;
}

static int cmd_batt(int argc, char **argv) {
    if (argc < 3) { printf("usage: batt hr|pwr <pct>\n"); return 1; }
    device_t *d = dev_by_label(argv[1]);
    if (!d) return 1;
    d->batt_pct = (uint8_t)atoi(argv[2]);
    if (d->sub_batt) notify(d, h_batt, &d->batt_pct, 1);
    return 0;
}

static int cmd_jitter(int argc, char **argv) {
    if (argc < 2) { printf("jitter %s\n", sim.jitter ? "on" : "off"); return 0; }
    sim.jitter = !strcmp(argv[1], "on");
    return 0;
}

static int cmd_auto(int argc, char **argv) { sim.auto_mode = true; printf("auto ride on\n"); return 0; }

static int cmd_onoff(int argc, char **argv) {
    if (argc < 2) { printf("usage: %s hr|pwr\n", argv[0]); return 1; }
    device_t *d = dev_by_label(argv[1]);
    if (!d) { printf("no such device\n"); return 1; }
    bool on = !strcmp(argv[0], "on");
    d->enabled = on;
    if (on) adv_start(d);
    else {
        adv_stop(d);
        if (d->conn != BLE_HS_CONN_HANDLE_NONE)
            ble_gap_terminate(d->conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    printf("%s %s\n", d->label, on ? "on" : "off");
    return 0;
}

static int cmd_role(int argc, char **argv) {
    if (argc < 2) {
        printf("role: %s\n", role_mask == 3 ? "both" : role_mask == 1 ? "hr" : "pwr");
        return 0;
    }
    uint8_t m = !strcmp(argv[1], "hr") ? 1 : !strcmp(argv[1], "pwr") ? 2
              : !strcmp(argv[1], "both") ? 3 : 0;
    if (!m) { printf("usage: role hr|pwr|both\n"); return 1; }
    role_mask = m;
    role_save();
    for (int i = 0; i < DEV_COUNT; ++i) {
        char *a[2] = { (m >> i) & 1 ? "on" : "off", (char *)devs[i].label };
        cmd_onoff(2, a);
    }
    return 0;
}

// Machine-readable state for tools/sensor-sim/webui.py.
static int cmd_state(int argc, char **argv) {
    printf("{\"t\":%lu,\"auto\":%d,\"jitter\":%d,\"hr\":%d,\"contact\":%d,\"watts\":%d,\"cad\":%d,\"bal\":%d,"
           "\"out\":{\"hr\":%d,\"watts\":%d,\"cad\":%d,\"bal\":%d},\"role\":%u,\"devs\":[",
           (unsigned long)sim.t, sim.auto_mode, sim.jitter, sim.hr_bpm, sim.contact, sim.watts, sim.cad_rpm,
           sim.balance_pct, sim.out_hr, sim.out_watts, sim.out_cad, sim.out_bal, role_mask);
    for (int i = 0; i < DEV_COUNT; ++i) {
        device_t *d = &devs[i];
        printf("%s{\"id\":\"%s\",\"name\":\"%s\",\"addr\":\"%02X:%02X:%02X:%02X:%02X:%02X\","
               "\"enabled\":%d,\"adv\":%d,\"conn\":%d,\"sub\":%d,\"batt\":%u}",
               i ? "," : "", d->label, d->adv_name, d->addr.val[5], d->addr.val[4], d->addr.val[3],
               d->addr.val[2], d->addr.val[1], d->addr.val[0], d->enabled,
               ble_gap_ext_adv_active((uint8_t)i), d->conn != BLE_HS_CONN_HANDLE_NONE,
               d->sub_meas, d->batt_pct);
    }
    printf("]}\n");
    return 0;
}

static int cmd_status(int argc, char **argv) {
    printf("t=%lus auto=%d hr=%d contact=%d watts=%d cad=%d bal=%d revs=%u\n",
           (unsigned long)sim.t, sim.auto_mode, sim.hr_bpm, sim.contact, sim.watts,
           sim.cad_rpm, sim.balance_pct, sim.crank_revs);
    for (int i = 0; i < DEV_COUNT; ++i) {
        device_t *d = &devs[i];
        printf("%-4s %-14s %02X:%02X:%02X:%02X:%02X:%02X %s adv=%d conn=%d sub=%d batt=%u%%\n",
               d->label, d->adv_name, d->addr.val[5], d->addr.val[4], d->addr.val[3],
               d->addr.val[2], d->addr.val[1], d->addr.val[0],
               d->enabled ? "on " : "off", ble_gap_ext_adv_active((uint8_t)i),
               d->conn != BLE_HS_CONN_HANDLE_NONE, d->sub_meas, d->batt_pct);
    }
    return 0;
}

static void console_init(void) {
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "sim>";
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl));
    esp_console_register_help_command();
    const esp_console_cmd_t cmds[] = {
        {.command = "hr",      .help = "hr <bpm>            pin heart rate",          .func = cmd_set},
        {.command = "pwr",     .help = "pwr <watts>         pin power",               .func = cmd_set},
        {.command = "cad",     .help = "cad <rpm>           pin cadence (0 = stopped)", .func = cmd_set},
        {.command = "bal",     .help = "bal <left%>         pedal balance",           .func = cmd_set},
        {.command = "contact", .help = "contact on|off      strap skin contact",      .func = cmd_contact},
        {.command = "batt",    .help = "batt hr|pwr <pct>   battery level",           .func = cmd_batt},
        {.command = "jitter",  .help = "jitter on|off       flutter around set values (manual mode)", .func = cmd_jitter},
        {.command = "auto",    .help = "scripted interval ride (default)",            .func = cmd_auto},
        {.command = "off",     .help = "off hr|pwr          drop + stop advertising", .func = cmd_onoff},
        {.command = "on",      .help = "on hr|pwr           advertise again",         .func = cmd_onoff},
        {.command = "role",    .help = "role hr|pwr|both   which device(s) this board plays (saved)", .func = cmd_role},
        {.command = "state",   .help = "state as one JSON line (for webui.py)",             .func = cmd_state},
        {.command = "status",  .help = "show state",                                  .func = cmd_status},
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; ++i)
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    ESP_ERROR_CHECK(esp_console_start_repl(repl));
}

void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    role_load();

    ESP_ERROR_CHECK(nimble_port_init());
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    ESP_ERROR_CHECK(ble_gatts_count_cfg(gatt_svcs));
    ESP_ERROR_CHECK(ble_gatts_add_svcs(gatt_svcs));
    ble_svc_gap_device_name_set("sensor-sim");
    nimble_port_freertos_init(host_task);

    const esp_timer_create_args_t ta = {.callback = tick, .name = "sim"};
    esp_timer_handle_t th;
    ESP_ERROR_CHECK(esp_timer_create(&ta, &th));
    ESP_ERROR_CHECK(esp_timer_start_periodic(th, 1000000));

    console_init();
}
