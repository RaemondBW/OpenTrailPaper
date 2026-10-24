// The phone link (docs/ble-link.md). See link.h for the shape of it.
//
// Threads:
//   NimBLE host task  RxCb::onWrite reassembles fragments into rxAsm (PSRAM)
//                     and copies each whole message, tagged with the
//                     connection epoch, into rxRing. Nothing else.
//                     onAdmitted/onDisconnected bump the epoch.
//   link task         decodes into one static PhoneMsg, runs the request,
//                     answers, streams downloads, pushes, and is the only
//                     notify() on TX. Every per-connection field below
//                     (session, upload, download, deferred) is its alone.
//   any task          push()/pushMediaCommand() set atomics for it.

#include "link.h"

#include <Arduino.h>
#include <NimBLEDevice.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <freertos/ringbuf.h>
#include <atomic>
#include <math.h>
#include <string.h>

#include <pb.h>
#include <pb_decode.h>
#include <pb_encode.h>
#include "proto/link.pb.h"

#include "ams_client.h"
#include "ble_sensors.h"
#include "ble_shared.h"
#include "config.h"
#include "dash_config.h"
#include "dash_layout.h"
#include "diag.h"
#include "gps_service.h"
#include "link_frame.h"
#include "map_store.h"
#include "media.h"
#include "mesh_proto.h"
#include "mesh_service.h"
#include "radar.h"
#include "ride_recorder.h"
#include "ride_state.h"
#include "routes.h"
#include "sd_bus.h"
#include "settings.h"
#include "speed_source.h"
#include "tile_versions.h"
#include "usb_storage.h"
#include "workout_service.h"

namespace {

const char* SVC_LINK = "b1c51000-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_RX = "b1c51001-9e0f-4b7a-9c6d-1f2e3a4b5c6d";
const char* CHR_TX = "b1c51002-9e0f-4b7a-9c6d-1f2e3a4b5c6d";

// Limits (DeviceHello and docs/ble-link.md "Transfers").
constexpr size_t RX_RING_BYTES = 32 * 1024;         // > upload window + control
constexpr uint32_t UPLOAD_WINDOW = 16 * 1024;
constexpr uint32_t UPLOAD_ACK_EVERY = 4 * 1024;
constexpr uint32_t UPLOAD_STALL_MS = 20000;
constexpr uint32_t DOWNLOAD_WINDOW = 8 * 1024;
constexpr uint32_t DOWNLOAD_ACK_TIMEOUT_MS = 10000;
constexpr uint32_t LIVE_PERIOD_MS = 1000;
constexpr uint32_t SENSOR_PUSH_MS = 2000;
constexpr uint32_t ROUTE_MAX = 256 * 1024;          // as the legacy ROUTE buffer
constexpr uint32_t WORKOUT_MAX = 16 * 1024;         // as the legacy WORKOUT buffer
constexpr uint32_t AGNSS_MAX = 32 * 1024;           // gps_service's assistance buffer
constexpr uint32_t FIRMWARE_MAX = 8 * 1024 * 1024;  // larger than any app partition
constexpr uint32_t ART_MAX_SIDE = 320;
constexpr int LIST_MAX = 512;                       // files per FileList walk
constexpr int FILE_PAGE = 16;                       // link.options max_count
constexpr int TILE_PAGE = 32;
constexpr int MESH_HISTORY_PAGE = 4;
constexpr int MESH_NODES_PAGE = 8;
constexpr int DEFERRED_MAX = 8;

// --- Host task side -------------------------------------------------------
NimBLECharacteristic* txChr = nullptr;
uint8_t* rxAsmBuf = nullptr;            // PSRAM, LINK_MAX_MESSAGE
link_reasm_t rxAsm;
RingbufHandle_t rxRing = nullptr;
StaticRingbuffer_t rxRingStruct;
std::atomic<uint32_t> rxRingDrops{0};
std::atomic<uint32_t> rxFrameDrops{0};
std::atomic<uint8_t> rxLastDrop{0};

// Bumped on admission and on disconnect: every link session is one epoch.
std::atomic<uint32_t> connEpoch{1};
// The epoch whose app said Hello (0 none). active() compares it live, so a
// disconnect ends link mode at once, from the host task, with no race
// against the link task.
std::atomic<uint32_t> helloEpoch{0};

std::atomic<uint32_t> pushMask{0};
std::atomic<uint8_t> pendingMediaCmd{0};

// --- Link task side -------------------------------------------------------
otp_link_PhoneMsg* inMsg = nullptr;     // PSRAM: decoded request
otp_link_DeviceMsg* outMsg = nullptr;   // PSRAM: message being built
uint8_t* txBuf = nullptr;               // PSRAM: encoded DeviceMsg
uint8_t* ioBuf = nullptr;               // PSRAM: 4 KB SD read scratch
otp_link_FileEntry* listBuf = nullptr;  // PSRAM: one directory walk
char* dashPending = nullptr;            // PSRAM: a SetDash waiting for the card

struct Session {
    uint32_t epoch = 0;
    bool hello = false;
    uint8_t txCounter = 0;
    uint32_t topics = 0;            // bit per otp_link_LiveTopic
    bool liveKick = false;
    uint32_t liveSeq = 0;
    uint32_t lastLiveMs = 0;
    uint32_t mediaVerSeen = 0;
    bool mediaHandler = false;      // phone listed FEATURE_MEDIA_COMMAND_HANDLER
    bool sensorsStreaming = false;
    uint32_t lastSensorPushMs = 0;
    uint32_t dashVerSeen = 0;
    uint32_t workoutVerSeen = 0;
    uint32_t lastWorkoutPushMs = 0;
    uint32_t trackVerSent = 0;
    bool routeLoaded = false;       // the last TARGET_ROUTE parsed OK
    bool txFailLogged = false;
};
Session S;

struct Upload {
    bool active = false;      // begun, not finished
    bool receiving = false;   // chunks expected
    bool commitDue = false;   // size + CRC checked; saving waits for the card
    uint32_t epoch = 0;
    uint32_t xfer = 0;
    uint32_t endReq = 0;
    otp_link_Target target = otp_link_Target_TARGET_NONE;
    char name[64] = "";
    char version[tile_versions::VER_MAX] = "";
    uint32_t size = 0, crc = 0, got = 0, acked = 0;
    uint32_t startMs = 0, lastDataMs = 0;
    otp_link_ArtFormat artFormat = otp_link_ArtFormat_ART_GRAY8;
    uint32_t artW = 0, artH = 0;
    uint8_t* buf = nullptr;
};
Upload up;

struct Download {
    bool active = false;
    uint32_t xfer = 0;
    uint32_t size = 0, next = 0, acked = 0, window = 0, crc = 0;
    uint32_t startMs = 0, lastAckMs = 0, startOffset = 0;
    char label[64] = "";
    File f;
};
Download dl;

struct Deferred {
    uint32_t req;
    otp_link_ListKind kind;   // LIST_NONE = the pending SetDash
};
Deferred deferred[DEFERRED_MAX];
int deferredCount = 0;

// ---------------------------------------------------------------------------
// Small helpers

// Copy at most cap-1 bytes of src, never splitting a UTF-8 sequence.
void copyStr(char* dst, size_t cap, const char* src) {
    if (!cap) return;
    if (!src) src = "";
    size_t n = strnlen(src, cap - 1);
    if (src[n] != 0)
        while (n > 0 && ((uint8_t)src[n] & 0xC0) == 0x80) --n;
    memcpy(dst, src, n);
    dst[n] = 0;
}

// A bare file name: no path, no dotfile, not empty.
bool safeName(const char* n) {
    if (!n || !n[0] || n[0] == '.') return false;
    return !strchr(n, '/') && !strchr(n, '\\');
}

bool hexName(const char* n) {
    if (!n || !n[0]) return false;
    for (const char* p = n; *p; ++p)
        if (!isxdigit((unsigned char)*p)) return false;
    return strlen(n) < 24;
}

size_t varintLen(uint32_t v) {
    size_t n = 1;
    while (v >= 0x80) { v >>= 7; ++n; }
    return n;
}

const char* targetName(otp_link_Target t) {
    switch (t) {
    case otp_link_Target_TARGET_FIRMWARE: return "firmware";
    case otp_link_Target_TARGET_MAP: return "map";
    case otp_link_Target_TARGET_MAP_TILE: return "tile";
    case otp_link_Target_TARGET_MAP_POI: return "poi";
    case otp_link_Target_TARGET_ROUTE: return "route";
    case otp_link_Target_TARGET_WORKOUT: return "workout";
    case otp_link_Target_TARGET_ALBUM_ART: return "album art";
    case otp_link_Target_TARGET_AGNSS: return "agnss";
    default: return "?";
    }
}

const char* resultName(otp_link_ResultCode c) {
    switch (c) {
    case otp_link_ResultCode_RESULT_OK: return "ok";
    case otp_link_ResultCode_RESULT_UNSUPPORTED: return "unsupported";
    case otp_link_ResultCode_RESULT_BAD_REQUEST: return "bad request";
    case otp_link_ResultCode_RESULT_BUSY: return "busy";
    case otp_link_ResultCode_RESULT_RECORDING: return "recording";
    case otp_link_ResultCode_RESULT_NO_SD: return "no sd";
    case otp_link_ResultCode_RESULT_NOT_FOUND: return "not found";
    case otp_link_ResultCode_RESULT_NO_MEMORY: return "no memory";
    case otp_link_ResultCode_RESULT_IO_ERROR: return "io error";
    case otp_link_ResultCode_RESULT_PARSE_ERROR: return "parse error";
    case otp_link_ResultCode_RESULT_CRC_MISMATCH: return "crc mismatch";
    case otp_link_ResultCode_RESULT_TOO_LARGE: return "too large";
    case otp_link_ResultCode_RESULT_REFUSED: return "refused";
    case otp_link_ResultCode_RESULT_TIMEOUT: return "timeout";
    case otp_link_ResultCode_RESULT_CANCELLED: return "cancelled";
    }
    return "?";
}

// ---------------------------------------------------------------------------
// Sending (link task only)

// Starts a message in outMsg.
otp_link_DeviceMsg& msg(uint32_t req, pb_size_t tag) {
    memset(outMsg, 0, sizeof(*outMsg));
    outMsg->req = req;
    outMsg->which_body = tag;
    return *outMsg;
}

bool sessionLive() {
    return S.hello && ble_shared::phoneAdmitted() && connEpoch.load() == S.epoch;
}

// outMsg -> fragments -> TX. One message at a time and never interleaved;
// priority is decided by what the loop chooses to send next. A notify the
// stack can't take (out of buffers) is retried like the legacy sendChunk;
// one that never goes ends the message (the phone's counter check drops the
// partial, and the next message starts with FIRST).
constexpr uint32_t NOTIFY_STALL_MS = 3000;

bool send(bool bulk = false) {
    if (!sessionLive() || !txChr) return false;
    pb_ostream_t os = pb_ostream_from_buffer(txBuf, otp_link_DeviceMsg_size);
    if (!pb_encode(&os, otp_link_DeviceMsg_fields, outMsg)) {
        diag::log("link: encode failed (body %u): %s", (unsigned)outMsg->which_body,
                  PB_GET_ERROR(&os));
        return false;
    }
    const size_t len = os.bytes_written;
    const uint16_t mtu = ble_shared::mtu();
    uint8_t frag[256];
    size_t maxFrag = mtu > 3 ? (size_t)mtu - 3 : 20;
    if (maxFrag > sizeof(frag)) maxFrag = sizeof(frag);
    const uint16_t handle = ble_shared::phoneHandle();
    size_t off = 0;
    do {
        if (!sessionLive()) return false;
        size_t used = 0;
        const size_t n = link_frag_build(txBuf, len, off, S.txCounter, frag, maxFrag, &used);
        // The controller drains a few packets per connection event, and the
        // interval can sit at 300 ms until a bulk request takes effect, so
        // wait in time, not tries: up to 3 s (under the 4 s supervision
        // timeout) while the session lasts.
        bool ok = false;
        const uint32_t t0 = millis();
        while (sessionLive()) {
            if (txChr->notify(frag, n, handle)) { ok = true; break; }
            if (millis() - t0 > NOTIFY_STALL_MS) break;
            vTaskDelay(pdMS_TO_TICKS(8));
        }
        if (!ok) {
            if (!S.txFailLogged)
                diag::log("link: notify failed (body %u, %u/%u bytes sent)",
                          (unsigned)outMsg->which_body, (unsigned)off, (unsigned)len);
            S.txFailLogged = true;
            return false;
        }
        S.txCounter = (uint8_t)((S.txCounter + 1) & LINK_HDR_COUNTER);
        off += used;
    } while (off < len);
    // Bulk: a breath between chunks so the controller's buffers drain and a
    // control message queued meanwhile goes next.
    if (bulk) vTaskDelay(pdMS_TO_TICKS(2));
    return true;
}

void sendResult(uint32_t req, otp_link_ResultCode code, const char* detail = nullptr) {
    otp_link_DeviceMsg& m = msg(req, otp_link_DeviceMsg_result_tag);
    m.body.result.code = code;
    copyStr(m.body.result.detail, sizeof(m.body.result.detail), detail);
    send();
}

void sendTransferDone(uint32_t req, uint32_t xfer, otp_link_ResultCode code,
                      const char* detail, uint32_t size = 0, uint32_t crc = 0) {
    otp_link_DeviceMsg& m = msg(req, otp_link_DeviceMsg_transfer_done_tag);
    m.body.transfer_done.xfer = xfer;
    m.body.transfer_done.code = code;
    copyStr(m.body.transfer_done.detail, sizeof(m.body.transfer_done.detail), detail);
    m.body.transfer_done.size = size;
    m.body.transfer_done.crc32 = crc;
    send();
}

void sendTransferCancel(uint32_t xfer, otp_link_ResultCode reason) {
    otp_link_DeviceMsg& m = msg(0, otp_link_DeviceMsg_transfer_cancel_tag);
    m.body.transfer_cancel.xfer = xfer;
    m.body.transfer_cancel.reason = reason;
    send();
}

// ---------------------------------------------------------------------------
// State snapshots -> messages

void sendSettings(uint32_t req) {
    otp_link_Settings& s = msg(req, otp_link_DeviceMsg_settings_tag).body.settings;
    s.has_ftp_w = true;      s.ftp_w = settings::ftpWatts();
    s.has_tz_min = true;     s.tz_min = settings::tzMinutes();
    s.has_use_miles = true;  s.use_miles = settings::useMiles();
    s.has_backlight = true;  s.backlight = (uint32_t)settings::backlight();
    s.has_clock_24h = true;  s.clock_24h = settings::clock24h();
    s.has_usb_drive = true;  s.usb_drive = settings::usbDrive();
    s.has_wheel_mm = true;   s.wheel_mm = (uint32_t)settings::wheelCircMm();
    send();
}

void sendMapLayers(uint32_t req) {
    msg(req, otp_link_DeviceMsg_map_layers_tag).body.map_layers.mask = settings::mapLayers();
    send();
}

void sendDash(uint32_t req) {
    otp_link_DeviceMsg& m = msg(req, otp_link_DeviceMsg_dash_tag);
    dash_config::currentText(m.body.dash.text, sizeof(m.body.dash.text));
    m.body.dash.text[sizeof(m.body.dash.text) - 1] = 0;
    send();
}

void sendWorkoutStatus(uint32_t req) {
    WorkoutView v;
    workout_service::view(v);
    otp_link_WorkoutStatus& w = msg(req, otp_link_DeviceMsg_workout_status_tag).body.workout_status;
    w.loaded = v.loaded;
    w.running = v.running;
    w.paused = v.paused;
    w.done = v.done;
    w.segment = v.segIdx > 0 ? (uint32_t)v.segIdx : 0;
    w.segment_count = v.segCount > 0 ? (uint32_t)v.segCount : 0;
    w.elapsed_s = v.elapsedSec;
    w.total_s = v.totalSec;
    w.target_w = v.targetW;
    w.ftp_w = v.ftpW;
    w.pause_each = settings::workoutPauseEachBlock();
    copyStr(w.name, sizeof(w.name), v.name);
    S.lastWorkoutPushMs = millis();
    S.workoutVerSeen = workout_service::version();
    send();
}

void sendMediaTrack(uint32_t req) {
    const MediaState& st = media::get();
    otp_link_MediaTrack& t = msg(req, otp_link_DeviceMsg_media_track_tag).body.media_track;
    copyStr(t.title, sizeof(t.title), st.title);
    copyStr(t.artist, sizeof(t.artist), st.artist);
    copyStr(t.album, sizeof(t.album), st.album);
    send();
}

void sendSensorList(uint32_t req) {
    static ble_sensors::Candidate cs[10];   // link task only
    const int n = ble_sensors::getCandidates(cs, 10);
    otp_link_SensorList& l = msg(req, otp_link_DeviceMsg_sensor_list_tag).body.sensor_list;
    for (int i = 0; i < n && i < 10; ++i) {
        otp_link_Sensor& o = l.sensors[l.sensors_count++];
        o.kinds = cs[i].kindsMask;
        o.connected = cs[i].connected;
        o.paired = cs[i].paired;
        o.rssi = cs[i].rssi;
        char name[sizeof(cs[i].name) + 1];
        memcpy(name, cs[i].name, sizeof(cs[i].name));
        name[sizeof(cs[i].name)] = 0;
        copyStr(o.name, sizeof(o.name), name);
        char addr[sizeof(cs[i].addr) + 1];
        memcpy(addr, cs[i].addr, sizeof(cs[i].addr));
        addr[sizeof(cs[i].addr)] = 0;
        copyStr(o.address, sizeof(o.address), addr);
    }
    S.lastSensorPushMs = millis();
    send();
}

// ---------------------------------------------------------------------------
// Live frames

inline uint32_t clampU(double v, double hi) {
    return v <= 0 ? 0 : v >= hi ? (uint32_t)hi : (uint32_t)(v + 0.5);
}
inline int32_t roundI(double v) {
    return (int32_t)(v < 0 ? v - 0.5 : v + 0.5);
}

void sendLiveFrame() {
    RideState s = g_state.snapshot();
    ble_server::LiveView v;
    ble_shared::readLiveView(v);
    const uint32_t now = millis();
    const uint32_t t = S.topics;
    otp_link_LiveFrame& f = msg(0, otp_link_DeviceMsg_live_tag).body.live;
    f.seq = S.liveSeq++;

    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_RIDE)) {
        radarExpire(s.radar, now);
        // The dashboard's freshness rule (ui_dashboard: kSensorStaleMs).
        constexpr uint32_t kStale = 15000;
        const bool hrLive = s.hrConnected && now - s.hrMs <= kStale;
        const bool pwLive = s.powerConnected && now - s.powerMs <= kStale;
        const bool cadLive = s.cadenceConnected && now - s.cadenceMs <= kStale;
        const bool routeOn = routes::active();
        otp_link_RideState& r = f.ride;
        f.has_ride = true;
        r.fix = s.gpsFix;
        r.recording = s.recording;
        r.auto_paused = s.ridePaused;
        r.route_active = routeOn;
        r.charging = s.charging;
        r.battery_pct = s.batteryPercent;
        r.sats = s.satellites;
        r.has_speed_dkmh = true;
        r.speed_dkmh = clampU(s.speedKmh * 10.0, 65535);
        r.speed_from_wheel = s.speedSource == speed_source::SENSOR;
        if (pwLive && s.powerW != 0xFFFF) { r.has_power_w = true; r.power_w = s.powerW; }
        if (pwLive && s.power3sW != 0xFFFF) { r.has_power3s_w = true; r.power3s_w = s.power3sW; }
        if (hrLive) { r.has_hr_bpm = true; r.hr_bpm = s.heartRateBpm; }
        if (cadLive) { r.has_cadence_rpm = true; r.cadence_rpm = s.cadenceRpm; }
        r.distance_m = clampU(s.distanceM, 4294967295.0);
        r.elapsed_s = s.elapsedS;
        r.moving_s = s.movingS;
        r.climb_m = clampU(s.climbedM, 4294967295.0);
        if (s.gradeValid) { r.has_grade_dpct = true; r.grade_dpct = roundI(s.gradePercent * 10.0); }
        if (s.baroValid || s.mapElevationValid) {
            r.has_altitude_m = true;
            r.altitude_m = roundI(s.baroValid ? s.baroAltM : s.mapElevationM);
        }
        r.altitude_baro = s.baroValid;
        if (routeOn) {
            r.has_route_left_m = true;
            r.route_left_m = clampU(routes::remainingKm() * 1000.0, 4294967295.0);
        }
        r.radar_connected = s.radar.connected;
        r.radar_live = s.radar.live;
        r.time_valid = s.timeValid;
    }
    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_STOPWATCH)) {
        otp_link_StopwatchState& w = f.stopwatch;
        f.has_stopwatch = true;
        const uint32_t el = v.swAccumMs + (v.swRunning ? now - v.swStartMs : 0);
        w.running = v.swRunning;
        w.shows_power = v.swShowPower;
        w.shows_cadence = v.swShowCadence;
        w.elapsed_ms = el;
        w.laps = v.swLaps;
        w.lap_ms = el - v.swLastLapEndMs;
        if (v.swLaps > 0) { w.has_last_split_ms = true; w.last_split_ms = v.swLastSplitMs; }
        if (v.swLapSpeedDkmh != 0xFFFF) { w.has_lap_speed_dkmh = true; w.lap_speed_dkmh = v.swLapSpeedDkmh; }
        if (v.swLapPowerW != 0xFFFF) { w.has_lap_power_w = true; w.lap_power_w = v.swLapPowerW; }
        if (v.swLapCadenceRpm != 0xFF) { w.has_lap_cadence_rpm = true; w.lap_cadence_rpm = v.swLapCadenceRpm; }
        // The page's lap table, newest first.
        const int maxRows = (int)(sizeof(w.recent) / sizeof(w.recent[0]));
        for (int i = 0; i < v.swRows && i < ble_server::LiveView::LIVE_SW_ROWS && i < maxRows &&
                        i < v.swLaps;
             ++i) {
            otp_link_Lap& l = w.recent[w.recent_count++];
            l.number = (uint32_t)(v.swLaps - i);
            l.split_ms = v.swRowSplitMs[i];
            if (v.swRowSpeedDkmh[i] != 0xFFFF) { l.has_speed_dkmh = true; l.speed_dkmh = v.swRowSpeedDkmh[i]; }
            if (v.swRowCadenceRpm[i] != 0xFF) { l.has_cadence_rpm = true; l.cadence_rpm = v.swRowCadenceRpm[i]; }
            if (v.swRowPowerW[i] != 0xFFFF) { l.has_power_w = true; l.power_w = v.swRowPowerW[i]; }
        }
    }
    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_VIEW)) {
        f.has_view = true;
        if (v.page != 0xFF) { f.view.has_page = true; f.view.page = v.page; }
        f.view.page_count = v.pageCount;
        f.view.screen = v.screen == 0 ? otp_link_Screen_SCREEN_DASH
                      : v.screen == 1 ? otp_link_Screen_SCREEN_MAP
                                      : otp_link_Screen_SCREEN_OTHER;
    }
    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_POSITION)) {
        // The point the device map is drawn around, in its order
        // (ui_dashboard's map render): own fix, the phone's recent location,
        // last known fix, the saved position.
        double lat, lon;
        const bool phoneRecent = s.phoneFixValid && millis() - s.phoneFixMs < 15000;
        f.has_position = true;
        if (s.gpsFix || (!phoneRecent && s.everHadFix)) {
            lat = s.latitude;
            lon = s.longitude;
            if (s.gpsFix && s.speedKmh > 3.0f) {
                f.position.has_course_ddeg = true;
                f.position.course_ddeg = clampU(s.courseDeg * 10.0, 3599);
            }
            f.position.current = s.gpsFix;
        } else if (phoneRecent) {
            lat = s.phoneLat;
            lon = s.phoneLon;
            if (s.phoneCourseValid) {
                f.position.has_course_ddeg = true;
                f.position.course_ddeg = clampU(s.phoneCourseDeg * 10.0, 3599);
            }
            f.position.current = true;
            f.position.from_phone = true;
        } else {
            settings::lastPosition(lat, lon);
        }
        f.position.lat_e7 = (int32_t)(lat * 1e7);
        f.position.lon_e7 = (int32_t)(lon * 1e7);
    }
    if ((t & (1u << otp_link_LiveTopic_LIVE_TOPIC_RADAR)) && s.radar.connected) {
        if (!(t & (1u << otp_link_LiveTopic_LIVE_TOPIC_RIDE))) radarExpire(s.radar, now);
        f.has_radar = true;
        f.radar.live = s.radar.live;
        int cnt = s.radar.count > RADAR_MAX_TARGETS ? RADAR_MAX_TARGETS : s.radar.count;
        if (cnt > 8) cnt = 8;
        for (int i = 0; i < cnt; ++i) f.radar.distances_m[i] = s.radar.targets[i].distanceM;
        f.radar.distances_m_count = (pb_size_t)cnt;
    }
    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_MUSIC)) {
        // A reference, not a copy: AMS writes it on the host task, and a frame
        // built across an update is off by one report until the next frame
        // (sent at once on a media version change).
        const MediaState& m = media::get();
        f.has_music = true;
        f.music.has_player = m.present;
        f.music.playing = m.present && m.playing;
        f.music.from_ams = ams::active();
        f.music.elapsed_ms = m.present ? m.positionMs(now) : 0;
        f.music.duration_ms = m.present ? (uint32_t)m.durSec * 1000 : 0;
        f.music.rate_pct = m.present && m.playing ? roundI(m.rate * 100.0f) : 0;
    }
    send();
}

// ---------------------------------------------------------------------------
// Mesh topics

void sendMeshState(uint32_t req) {
    otp_link_MeshState& m = msg(req, otp_link_DeviceMsg_mesh_state_tag).body.mesh_state;
    m.enabled = mesh_service::enabled();
    m.radio_ok = mesh_service::radioOk();
    m.channel_follows_preset = mesh_service::channelFollowsPreset();
    m.node_num = mesh_service::nodeNum();
    m.freq_hz = (uint32_t)(mesh_service::frequencyMHz() * 1e6f + 0.5f);
    m.psk_index = mesh_service::channelPskIndex();
    m.node_count = (uint32_t)mesh_service::nodeCount();
    const int un = mesh_service::unreadCount();
    m.unread = un > 0 ? (uint32_t)un : 0;
    copyStr(m.channel_name, sizeof(m.channel_name), mesh_service::channelName());
    copyStr(m.short_name, sizeof(m.short_name), mesh_service::shortName());
    copyStr(m.long_name, sizeof(m.long_name), mesh_service::longName());
    m.preset = mesh_service::presetIndex();
    m.region = mesh_service::regionIndex();
    m.tx_power_dbm = mesh_service::txPowerDbm();
    m.slot_override = mesh_service::freqSlot();
    m.slot_count = mesh_service::slotCount();
    m.active_slot = mesh_service::activeSlot();
    send();
}

void sendMeshHistory(uint32_t req) {
    const int n = mesh_service::messageCount();
    int i = 0;
    do {
        otp_link_MeshHistory& h = msg(req, otp_link_DeviceMsg_mesh_history_tag).body.mesh_history;
        for (; i < n && h.messages_count < MESH_HISTORY_PAGE; ++i) {
            mesh_service::Message m;
            if (!mesh_service::messageAt(i, m)) continue;
            otp_link_MeshMessage& o = h.messages[h.messages_count++];
            o.id = m.id;
            o.from = m.from;
            o.to = m.to;
            o.utc = m.utc;
            o.age_ms = millis() - m.ms;
            o.rssi = m.rssi;
            o.snr = m.snr;
            if (m.hops != mesh_service::HOPS_UNKNOWN && m.hops <= 7) { o.has_hops = true; o.hops = m.hops; }
            o.outgoing = m.outgoing;
            o.delivery = m.status <= 3 ? (otp_link_MeshDelivery)m.status
                                       : otp_link_MeshDelivery_MESH_DELIVERY_PENDING;
            copyStr(o.text, sizeof(o.text), m.text);
            o.channel = m.channel;
        }
        h.last = i >= n;
        if (!send()) return;
    } while (i < n);
}

void sendMeshNodes(uint32_t req) {
    const int n = mesh_service::nodeCount();
    int i = 0;
    do {
        otp_link_MeshNodes& l = msg(req, otp_link_DeviceMsg_mesh_nodes_tag).body.mesh_nodes;
        for (; i < n && l.nodes_count < MESH_NODES_PAGE; ++i) {
            mesh_service::Node nd;
            if (!mesh_service::nodeAt(i, nd)) continue;
            otp_link_MeshNode& o = l.nodes[l.nodes_count++];
            o.num = nd.num;
            o.age_ms = millis() - nd.lastHeardMs;
            o.snr = nd.snr;
            o.rssi = nd.rssi;
            if (nd.hops != mesh_service::HOPS_UNKNOWN && nd.hops <= 7) { o.has_hops = true; o.hops = nd.hops; }
            copyStr(o.short_name, sizeof(o.short_name), nd.shortName);
            copyStr(o.long_name, sizeof(o.long_name), nd.longName);
            if (nd.hasPosition) {
                o.has_position = true;
                o.position.lat_e7 = (int32_t)(nd.latitude * 1e7);
                o.position.lon_e7 = (int32_t)(nd.longitude * 1e7);
                o.position.alt_m = nd.altitudeM;
                o.position.sats = nd.satsInView;
                o.position.precision_bits = nd.precisionBits;
                o.position.age_ms = millis() - nd.positionMs;
            }
        }
        l.last = i >= n;
        if (!send()) return;
    } while (i < n);
}

void sendMeshStats(uint32_t req) {
    const mesh_service::Stats st = mesh_service::stats();
    otp_link_MeshStats& m = msg(req, otp_link_DeviceMsg_mesh_stats_tag).body.mesh_stats;
    m.rx = st.rx;
    m.rx_dropped = st.rxDropped;
    m.rx_other_channel = st.rxOtherChannel;
    m.rx_duplicate = st.rxDuplicate;
    m.tx = st.tx;
    m.tx_failed = st.txFailed;
    m.acks_rx = st.acksRx;
    send();
}

void sendMeshPresets(uint32_t req) {
    otp_link_MeshPresets& l = msg(req, otp_link_DeviceMsg_mesh_presets_tag).body.mesh_presets;
    const int cap = (int)(sizeof(l.presets) / sizeof(l.presets[0]));
    for (int i = 0; i < mesh::PRESET_COUNT && i < cap; ++i) {
        const mesh::ModemPreset& mp = mesh::kPresets[i];
        otp_link_MeshPreset& o = l.presets[l.presets_count++];
        o.index = (uint32_t)i;
        o.sf = mp.sf;
        o.bw_khz_x10 = (uint32_t)(mp.bwKhz * 10.0f + 0.5f);
        o.cr = mp.cr;
        copyStr(o.name, sizeof(o.name), mp.name);
    }
    send();
}

void sendMeshChannels(uint32_t req) {
    otp_link_MeshChannels& l = msg(req, otp_link_DeviceMsg_mesh_channels_tag).body.mesh_channels;
    const int cap = (int)(sizeof(l.channels) / sizeof(l.channels[0]));
    for (int i = 0; i < mesh::MAX_CHANNELS && l.channels_count < cap; ++i) {
        mesh_service::ChannelInfo ci;
        if (!mesh_service::channelAt(i, ci)) continue;
        otp_link_MeshChannel& o = l.channels[l.channels_count++];
        o.index = ci.index;
        o.hash = ci.hash;
        copyStr(o.name, sizeof(o.name), ci.name);
        // The key goes to the phone because building a share QR needs it; the
        // link is the one the rider already trusts with everything else here.
        const size_t pl = ci.pskLen <= sizeof(o.psk.bytes) ? ci.pskLen : sizeof(o.psk.bytes);
        memcpy(o.psk.bytes, ci.psk, pl);
        o.psk.size = (pb_size_t)pl;
        o.share_location = ci.sharesLocation;
    }
    send();
}

void sendMeshRegions(uint32_t req) {
    otp_link_MeshRegions& l = msg(req, otp_link_DeviceMsg_mesh_regions_tag).body.mesh_regions;
    const int cap = (int)(sizeof(l.regions) / sizeof(l.regions[0]));
    for (int i = 0; i < mesh::REGION_COUNT && i < cap; ++i) {
        const mesh::Region& r = mesh::kRegions[i];
        otp_link_MeshRegion& o = l.regions[l.regions_count++];
        o.index = (uint32_t)i;
        o.start_hz = (uint32_t)(r.startMHz * 1e6f + 0.5f);
        o.end_hz = (uint32_t)(r.endMHz * 1e6f + 0.5f);
        o.spacing_hz = (uint32_t)(r.spacingMHz * 1e6f + 0.5f);
        o.power_limit_dbm = r.powerLimitDbm;
        copyStr(o.name, sizeof(o.name), r.name);
    }
    send();
}

void sendMeshTopic(uint32_t req, otp_link_MeshTopic t) {
    switch (t) {
    case otp_link_MeshTopic_MESH_TOPIC_STATE: sendMeshState(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_HISTORY: sendMeshHistory(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_NODES: sendMeshNodes(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_STATS: sendMeshStats(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_PRESETS: sendMeshPresets(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_CHANNELS: sendMeshChannels(req); break;
    case otp_link_MeshTopic_MESH_TOPIC_REGIONS: sendMeshRegions(req); break;
    default: sendResult(req, otp_link_ResultCode_RESULT_UNSUPPORTED, "mesh topic"); break;
    }
}

// The follow-up pushes the legacy protocol sent after each edit.
constexpr uint8_t MT_STATE = 1, MT_HISTORY = 2, MT_NODES = 4, MT_CHANNELS = 8;
void pushMeshTopics(uint8_t mask) {
    if (mask & MT_STATE) sendMeshState(0);
    if (mask & MT_HISTORY) sendMeshHistory(0);
    if (mask & MT_NODES) sendMeshNodes(0);
    if (mask & MT_CHANNELS) sendMeshChannels(0);
}

void onMesh(uint32_t req, const otp_link_MeshCommand& c) {
    using namespace mesh_service;
    uint8_t follow = 0;
    switch (c.which_op) {
    case otp_link_MeshCommand_get_tag:
        for (pb_size_t i = 0; i < c.op.get.topics_count; ++i) sendMeshTopic(req, c.op.get.topics[i]);
        return;
    case otp_link_MeshCommand_send_tag: {
        const uint32_t id = queueText(c.op.send.dest, c.op.send.text,
                                      (uint8_t)(c.op.send.channel > 255 ? 255 : c.op.send.channel));
        if (id) {
            msg(req, otp_link_DeviceMsg_mesh_queued_tag).body.mesh_queued.packet_id = id;
            send();
        } else {
            sendResult(req, otp_link_ResultCode_RESULT_REFUSED, "radio off or outbox full");
        }
        pushMeshTopics(MT_HISTORY | MT_STATE);   // show it in the app's list
        return;
    }
    case otp_link_MeshCommand_rename_tag:
        setNames(c.op.rename.long_name, c.op.rename.short_name);
        follow = MT_STATE;
        break;
    case otp_link_MeshCommand_radio_on_tag:
        setEnabled(c.op.radio_on);
        follow = MT_STATE;
        break;
    case otp_link_MeshCommand_primary_tag:
        if (c.op.primary.key_index > 255) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "key index");
            return;
        }
        setChannel(c.op.primary.name, (uint8_t)c.op.primary.key_index);
        follow = MT_STATE | MT_HISTORY | MT_NODES;
        break;
    case otp_link_MeshCommand_mark_read_tag:
        markAllRead();
        follow = MT_STATE;
        break;
    case otp_link_MeshCommand_set_channel_tag: {
        const otp_link_MeshChannelEdit& e = c.op.set_channel;
        if (e.index > 255 || e.psk.size > mesh::MAX_PSK_LEN ||
            !setPrivateChannel((uint8_t)e.index, e.name, e.psk.bytes, (uint8_t)e.psk.size)) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "channel");
            return;
        }
        follow = MT_CHANNELS | MT_STATE | MT_HISTORY;
        break;
    }
    case otp_link_MeshCommand_delete_channel_tag:
        if (c.op.delete_channel > 255 || !deletePrivateChannel((uint8_t)c.op.delete_channel)) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "channel");
            return;
        }
        follow = MT_CHANNELS | MT_STATE | MT_HISTORY;
        break;
    case otp_link_MeshCommand_share_tag:
        if (c.op.share.index > 255) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "channel");
            return;
        }
        setSharesLocation((uint8_t)c.op.share.index, c.op.share.on);
        follow = MT_CHANNELS;
        break;
    case otp_link_MeshCommand_preset_tag:
        if (c.op.preset >= (uint32_t)mesh::PRESET_COUNT) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "preset");
            return;
        }
        setPreset((uint8_t)c.op.preset);
        follow = MT_STATE;   // the frequency moves with the bandwidth
        break;
    case otp_link_MeshCommand_region_tag:
        if (c.op.region >= (uint32_t)mesh::REGION_COUNT) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "region");
            return;
        }
        setRegion((uint8_t)c.op.region);
        follow = MT_STATE | MT_HISTORY | MT_NODES;   // a new band is a new mesh
        break;
    case otp_link_MeshCommand_tx_power_dbm_tag:
        if (c.op.tx_power_dbm < 0 || c.op.tx_power_dbm > 127) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "tx power");
            return;
        }
        setTxPower((int8_t)c.op.tx_power_dbm);
        follow = MT_STATE;
        break;
    case otp_link_MeshCommand_slot_tag:
        if (c.op.slot > 255) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "slot");
            return;
        }
        setFreqSlot((uint8_t)c.op.slot);
        follow = MT_STATE | MT_HISTORY | MT_NODES;
        break;
    default:
        sendResult(req, otp_link_ResultCode_RESULT_UNSUPPORTED, "mesh op");
        return;
    }
    sendResult(req, otp_link_ResultCode_RESULT_OK);
    pushMeshTopics(follow);
}

// ---------------------------------------------------------------------------
// Lists

struct ListWalk {
    int n = 0;
    bool truncated = false;
};

void collectFile(const char* name, uint32_t size, void* ctx) {
    ListWalk* w = (ListWalk*)ctx;
    if (w->n >= LIST_MAX) { w->truncated = true; return; }
    otp_link_FileEntry& e = listBuf[w->n++];
    copyStr(e.name, sizeof(e.name), name);
    e.size = size;
}

void sendFileList(uint32_t req, otp_link_ListKind kind, ble_shared::Dir dir) {
    ListWalk w;
    ble_shared::listDir(dir, collectFile, &w);   // a missing directory is an empty list
    if (w.truncated) diag::log("link: list %d cut at %d files", (int)kind, LIST_MAX);
    int i = 0;
    do {
        otp_link_FileList& l = msg(req, otp_link_DeviceMsg_file_list_tag).body.file_list;
        l.kind = kind;
        for (; i < w.n && l.files_count < FILE_PAGE; ++i) l.files[l.files_count++] = listBuf[i];
        l.last = i >= w.n;
        if (!send()) return;
    } while (i < w.n);
}

void sendTileList(uint32_t req, otp_link_ListKind kind) {
    const bool poi = kind == otp_link_ListKind_LIST_MAP_POIS;
    ble_shared::TileListing tl;
    if (!ble_shared::lockTileList(poi, true, tl)) {
        sendResult(req, otp_link_ResultCode_RESULT_NO_MEMORY);
        return;
    }
    int i = 0;
    do {
        otp_link_TileList& l = msg(req, otp_link_DeviceMsg_tile_list_tag).body.tile_list;
        l.kind = kind;
        for (; i < tl.n && l.tiles_count < TILE_PAGE; ++i) {
            otp_link_TileEntry& e = l.tiles[l.tiles_count++];
            char id[25];
            memcpy(id, tl.ids[i], 24);
            id[24] = 0;
            copyStr(e.h3, sizeof(e.h3), id);
            if (tl.vers) {
                char ver[25];
                memcpy(ver, tl.vers[i], 24);
                ver[24] = 0;
                copyStr(e.version, sizeof(e.version), ver);
            }
        }
        l.last = i >= tl.n;
        if (!send()) break;
    } while (i < tl.n);
    ble_shared::unlockTileList();
}

void sendMapCoverage(uint32_t req) {
    static map_store::MapBounds mb[16];   // link task only
    const int n = map_store::listMaps(mb, 16);
    otp_link_MapCoverageList& l = msg(req, otp_link_DeviceMsg_map_coverage_tag).body.map_coverage;
    for (int i = 0; i < n && i < 16; ++i) {
        otp_link_MapCoverage& c = l.maps[l.maps_count++];
        c.south = mb[i].s;
        c.west = mb[i].w;
        c.north = mb[i].n;
        c.east = mb[i].e;
        c.builtin = mb[i].builtin;
    }
    send();
}

// Card state for rides and logs, as the legacy RIDES characteristic decides.
otp_link_ResultCode rideCardState() {
    if (ride_recorder::isRecording()) return otp_link_ResultCode_RESULT_RECORDING;
    if (!ride_recorder::sdMounted() || usb_storage::hostActive())
        return otp_link_ResultCode_RESULT_NO_SD;
    return otp_link_ResultCode_RESULT_OK;
}

otp_link_ResultCode cardState() {
    if (!ride_recorder::sdMounted() || usb_storage::hostActive())
        return otp_link_ResultCode_RESULT_NO_SD;
    return otp_link_ResultCode_RESULT_OK;
}

bool defer(uint32_t req, otp_link_ListKind kind) {
    if (deferredCount >= DEFERRED_MAX) return false;
    deferred[deferredCount++] = {req, kind};
    return true;
}

void onList(uint32_t req, otp_link_ListKind kind) {
    otp_link_ResultCode rc;
    switch (kind) {
    case otp_link_ListKind_LIST_RIDES:
        if ((rc = rideCardState()) != otp_link_ResultCode_RESULT_OK) break;
        sendFileList(req, kind, ble_shared::Dir::Rides);
        return;
    case otp_link_ListKind_LIST_LOGS:
        if ((rc = rideCardState()) != otp_link_ResultCode_RESULT_OK) break;
        diag::flushToSD();   // so today's file exists and is current
        sendFileList(req, kind, ble_shared::Dir::Logs);
        return;
    case otp_link_ListKind_LIST_ROUTES:
        if ((rc = cardState()) != otp_link_ResultCode_RESULT_OK) break;
        sendFileList(req, kind, ble_shared::Dir::Routes);
        return;
    case otp_link_ListKind_LIST_WORKOUTS:
        if ((rc = cardState()) != otp_link_ResultCode_RESULT_OK) break;
        sendFileList(req, kind, ble_shared::Dir::Workouts);
        return;
    case otp_link_ListKind_LIST_MAPS:
    case otp_link_ListKind_LIST_MAP_TILES:
    case otp_link_ListKind_LIST_MAP_POIS:
        // Walks the card: waits (like the legacy MAP lists) while the
        // recorder or a USB host owns it. serviceDeferred answers.
        if (!defer(req, kind)) {
            rc = otp_link_ResultCode_RESULT_BUSY;
            break;
        }
        return;
    default:
        rc = otp_link_ResultCode_RESULT_UNSUPPORTED;
        break;
    }
    sendResult(req, rc);
}

void onDelete(uint32_t req, const otp_link_DeleteRequest& d) {
    if (!safeName(d.name)) {
        sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "name");
        return;
    }
    otp_link_ResultCode rc = cardState();
    bool ok = false;
    if (rc == otp_link_ResultCode_RESULT_OK) {
        switch (d.kind) {
        case otp_link_ListKind_LIST_RIDES:
            if (ride_recorder::isRecording()) { rc = otp_link_ResultCode_RESULT_RECORDING; break; }
            ok = ble_shared::deleteRide(d.name);
            break;
        case otp_link_ListKind_LIST_ROUTES:
            ok = ble_shared::deleteRoute(d.name);
            break;
        case otp_link_ListKind_LIST_WORKOUTS:
            ok = ble_shared::deleteWorkout(d.name);
            break;
        default:
            rc = otp_link_ResultCode_RESULT_UNSUPPORTED;
            break;
        }
        if (rc == otp_link_ResultCode_RESULT_OK && !ok) rc = otp_link_ResultCode_RESULT_NOT_FOUND;
    }
    sendResult(req, rc);
}

// ---------------------------------------------------------------------------
// Uploads (phone -> device)

void freeUpload() {
    if (up.buf) heap_caps_free(up.buf);
    if (up.target == otp_link_Target_TARGET_FIRMWARE) ble_shared::setLinkUpdate(0, 0, 0);
    up = Upload();
}

void cancelUpload(otp_link_ResultCode reason, const char* why, bool tellPhone) {
    diag::log("link: upload %u (%s) cancelled: %s (%s) at %u/%u", (unsigned)up.xfer,
              targetName(up.target), resultName(reason), why, (unsigned)up.got,
              (unsigned)up.size);
    if (tellPhone) sendTransferCancel(up.xfer, reason);
    freeUpload();
}

void onUploadBegin(uint32_t req, const otp_link_UploadBegin& b) {
    otp_link_ResultCode rc = otp_link_ResultCode_RESULT_OK;
    const char* why = nullptr;
    uint32_t extra = 0;   // a NUL after a GPX
    if (up.active) {
        rc = otp_link_ResultCode_RESULT_BUSY;
        why = "another upload is running";
    } else if (b.size == 0) {
        rc = otp_link_ResultCode_RESULT_BAD_REQUEST;
        why = "empty";
    } else {
        switch (b.target) {
        case otp_link_Target_TARGET_FIRMWARE:
            if (ble_shared::legacyUpdateBusy()) { rc = otp_link_ResultCode_RESULT_BUSY; why = "update running"; }
            else if (b.size > FIRMWARE_MAX) rc = otp_link_ResultCode_RESULT_TOO_LARGE;
            // ~2 MB of PSRAM in ONE piece; the tile cache is exactly the
            // long-lived block that fragments that away.
            else map_store::releaseCache();
            break;
        case otp_link_Target_TARGET_MAP:
            if (b.name[0] && !safeName(b.name)) { rc = otp_link_ResultCode_RESULT_BAD_REQUEST; why = "name"; }
            break;
        case otp_link_Target_TARGET_MAP_TILE:
        case otp_link_Target_TARGET_MAP_POI:
            if (!hexName(b.name)) { rc = otp_link_ResultCode_RESULT_BAD_REQUEST; why = "h3 id"; }
            break;
        case otp_link_Target_TARGET_ROUTE:
            if (b.size > ROUTE_MAX) rc = otp_link_ResultCode_RESULT_TOO_LARGE;
            else if (b.name[0] && strchr(b.name, '/')) { rc = otp_link_ResultCode_RESULT_BAD_REQUEST; why = "name"; }
            extra = 1;
            break;
        case otp_link_Target_TARGET_WORKOUT:
            if (b.size > WORKOUT_MAX) rc = otp_link_ResultCode_RESULT_TOO_LARGE;
            else if (!safeName(b.name)) { rc = otp_link_ResultCode_RESULT_BAD_REQUEST; why = "name"; }
            break;
        case otp_link_Target_TARGET_ALBUM_ART: {
            const uint32_t w = b.art_width, h = b.art_height;
            const uint32_t want = b.art_format == otp_link_ArtFormat_ART_TONE5 ? (w * h + 2) / 3 : w * h;
            if (!w || !h || w > ART_MAX_SIDE || h > ART_MAX_SIDE || b.size != want ||
                (b.art_format != otp_link_ArtFormat_ART_GRAY8 &&
                 b.art_format != otp_link_ArtFormat_ART_TONE5)) {
                rc = otp_link_ResultCode_RESULT_BAD_REQUEST;
                why = "art size";
            }
            break;
        }
        case otp_link_Target_TARGET_AGNSS:
            if (b.size > AGNSS_MAX) rc = otp_link_ResultCode_RESULT_TOO_LARGE;
            break;
        default:
            rc = otp_link_ResultCode_RESULT_UNSUPPORTED;
            why = "target";
            break;
        }
    }
    uint8_t* buf = nullptr;
    if (rc == otp_link_ResultCode_RESULT_OK) {
        buf = (uint8_t*)heap_caps_malloc((size_t)b.size + extra, MALLOC_CAP_SPIRAM);
        if (!buf) rc = otp_link_ResultCode_RESULT_NO_MEMORY;
    }
    if (rc != otp_link_ResultCode_RESULT_OK) {
        diag::log("link: upload %u (%s, %u bytes) refused: %s%s%s", (unsigned)b.xfer,
                  targetName(b.target), (unsigned)b.size, resultName(rc), why ? " — " : "",
                  why ? why : "");
        sendResult(req, rc, why);
        return;
    }
    up = Upload();
    up.active = true;
    up.receiving = true;
    up.epoch = S.epoch;
    up.xfer = b.xfer;
    up.target = b.target;
    copyStr(up.name, sizeof(up.name), b.name);
    // A record this firmware can't keep is dropped, as on the legacy path.
    if (strlen(b.tile_version) < sizeof(up.version)) copyStr(up.version, sizeof(up.version), b.tile_version);
    up.size = b.size;
    up.crc = b.crc32;
    up.artFormat = b.art_format;
    up.artW = b.art_width;
    up.artH = b.art_height;
    up.buf = buf;
    up.startMs = up.lastDataMs = millis();
    if (up.target == otp_link_Target_TARGET_ROUTE) {
        // As the legacy START: a new route clears the pending maneuvers.
        routes::clearManeuvers();
        S.routeLoaded = false;
    }
    if (up.target == otp_link_Target_TARGET_FIRMWARE) ble_shared::setLinkUpdate(1, 0, up.size);
    ble_shared::noteBulk();
    diag::log("link: upload %u begin: %s '%s' %u bytes", (unsigned)up.xfer, targetName(up.target),
              up.name, (unsigned)up.size);
    otp_link_UploadReady& r = msg(req, otp_link_DeviceMsg_upload_ready_tag).body.upload_ready;
    r.xfer = up.xfer;
    r.window = UPLOAD_WINDOW;
    send();
}

void sendUploadAck() {
    otp_link_TransferAck& a = msg(0, otp_link_DeviceMsg_transfer_ack_tag).body.transfer_ack;
    a.xfer = up.xfer;
    a.received = up.got;
    up.acked = up.got;
    send();
}

void onChunk(const otp_link_Chunk& c) {
    if (!up.active || !up.receiving || c.xfer != up.xfer) return;   // a cancelled one's tail
    ble_shared::noteBulk();
    if (c.offset != up.got || c.data.size > up.size - up.got) {
        cancelUpload(otp_link_ResultCode_RESULT_BAD_REQUEST,
                     c.offset != up.got ? "offset gap" : "past size", true);
        return;
    }
    memcpy(up.buf + up.got, c.data.bytes, c.data.size);
    up.got += c.data.size;
    up.lastDataMs = millis();
    if (up.target == otp_link_Target_TARGET_FIRMWARE) ble_shared::setLinkUpdate(1, up.got, up.size);
    if (up.got - up.acked >= UPLOAD_ACK_EVERY) sendUploadAck();
}

void onUploadEnd(uint32_t req, const otp_link_UploadEnd& e) {
    if (!up.active || !up.receiving || e.xfer != up.xfer) {
        sendTransferDone(req, e.xfer, otp_link_ResultCode_RESULT_NOT_FOUND, "no such upload");
        return;
    }
    up.receiving = false;
    sendUploadAck();
    if (up.got != up.size) {
        diag::log("link: upload %u (%s) short: %u/%u", (unsigned)up.xfer, targetName(up.target),
                  (unsigned)up.got, (unsigned)up.size);
        sendTransferDone(req, up.xfer, otp_link_ResultCode_RESULT_BAD_REQUEST, "size mismatch");
        freeUpload();
        return;
    }
    const uint32_t crc = link_crc32(0, up.buf, up.size);
    if (crc != up.crc) {
        diag::log("link: upload %u (%s) CRC %08x, phone said %08x — dropped", (unsigned)up.xfer,
                  targetName(up.target), (unsigned)crc, (unsigned)up.crc);
        sendTransferDone(req, up.xfer, otp_link_ResultCode_RESULT_CRC_MISMATCH, nullptr);
        freeUpload();
        return;
    }
    up.endReq = req;
    up.commitDue = true;   // serviceUpload saves it as soon as the card allows
}

// The card gating each target's commit, as on the legacy path.
bool commitAllowed() {
    switch (up.target) {
    case otp_link_Target_TARGET_FIRMWARE:
    case otp_link_Target_TARGET_MAP:
    case otp_link_Target_TARGET_MAP_TILE:
    case otp_link_Target_TARGET_MAP_POI:
        return ble_shared::sdFree();
    case otp_link_Target_TARGET_ROUTE:
    case otp_link_Target_TARGET_WORKOUT:
        return !usb_storage::hostActive();
    default:
        return true;
    }
}

void commitUpload() {
    otp_link_ResultCode rc = otp_link_ResultCode_RESULT_OK;
    const char* detail = nullptr;
    bool reboot = false;
    switch (up.target) {
    case otp_link_Target_TARGET_FIRMWARE:
        ble_shared::setLinkUpdate(2, up.size, up.size);   // "installing" popup
        if (ble_shared::saveFirmwareImage(up.buf, up.size)) {
            detail = "saved, rebooting";
            reboot = true;
        } else {
            rc = otp_link_ResultCode_RESULT_IO_ERROR;
            detail = "SD write failed";
        }
        break;
    case otp_link_Target_TARGET_MAP:
        if (!map_store::saveAndActivate(up.name[0] ? up.name : "map.ebm", up.buf, up.size)) {
            rc = otp_link_ResultCode_RESULT_PARSE_ERROR;
            detail = "not a usable map, or SD write failed";
        }
        break;
    case otp_link_Target_TARGET_MAP_TILE:
    case otp_link_Target_TARGET_MAP_POI: {
        // saveTile tells a .poi from an .ebm by its magic; hold the phone to
        // the target it named.
        const bool isPoi = up.size >= 4 && memcmp(up.buf, "EPOI", 4) == 0;
        if (isPoi != (up.target == otp_link_Target_TARGET_MAP_POI)) {
            rc = otp_link_ResultCode_RESULT_PARSE_ERROR;
            detail = isPoi ? "a POI file sent as a tile" : "not a POI file";
        } else if (!map_store::saveTile(up.name, up.buf, up.size,
                                        up.version[0] ? up.version : nullptr)) {
            rc = otp_link_ResultCode_RESULT_PARSE_ERROR;
            detail = "not a usable tile, or SD write failed";
        }
        break;
    }
    case otp_link_Target_TARGET_ROUTE: {
        up.buf[up.size] = 0;   // allocated size + 1
        char name[routes::MAX_NAME + 8];
        copyStr(name, sizeof(name), up.name[0] ? up.name : "route.gpx");
        // Parse + write /routes/<name> here, on the link task — the legacy
        // ROUTE characteristic did this on the NimBLE host task.
        S.routeLoaded = routes::loadFromMemory(name, (const char*)up.buf, up.size);
        if (!S.routeLoaded) {
            rc = otp_link_ResultCode_RESULT_PARSE_ERROR;
            detail = "no track points";
        }
        break;
    }
    case otp_link_Target_TARGET_WORKOUT: {
        const char* reason = "";
        switch (ble_shared::saveWorkout(up.name, (const char*)up.buf, up.size, &reason)) {
        case ble_shared::WorkoutSave::Ok: break;
        case ble_shared::WorkoutSave::IoError:
            rc = otp_link_ResultCode_RESULT_IO_ERROR;
            detail = "SD write failed";
            break;
        case ble_shared::WorkoutSave::LoadFailed:
            rc = otp_link_ResultCode_RESULT_PARSE_ERROR;
            detail = reason;
            break;
        }
        break;
    }
    case otp_link_Target_TARGET_ALBUM_ART: {
        const bool tone = up.artFormat == otp_link_ArtFormat_ART_TONE5;
        if (!(tone ? media::beginToneArt((int)up.artW, (int)up.artH)
                   : media::beginArt((int)up.artW, (int)up.artH))) {
            rc = otp_link_ResultCode_RESULT_NO_MEMORY;
            break;
        }
        media::artData(up.buf, up.size);
        media::commitArt();   // dropped there if the title changed meanwhile
        break;
    }
    case otp_link_Target_TARGET_AGNSS:
        gps_service::agnssBegin();
        gps_service::agnssInject(up.buf, up.size);
        gps_service::agnssEnd();   // buffered; the GPS task feeds the module
        break;
    default:
        rc = otp_link_ResultCode_RESULT_UNSUPPORTED;
        break;
    }
    const uint32_t dt = millis() - up.startMs;
    diag::log("link: upload %u (%s '%s') done: %s, %u bytes in %.1fs", (unsigned)up.xfer,
              targetName(up.target), up.name, resultName(rc), (unsigned)up.size, dt / 1000.0f);
    // The phone that sent it may be gone (the commit waited for the card);
    // the work is done either way.
    if (up.epoch == S.epoch) sendTransferDone(up.endReq, up.xfer, rc, detail, up.size, up.crc);
    const bool wasFirmware = up.target == otp_link_Target_TARGET_FIRMWARE;
    if (wasFirmware && reboot) {
        heap_caps_free(up.buf);   // popup stays "installing" until the reboot
        up = Upload();
        ble_shared::rebootForUpdate();
        return;
    }
    freeUpload();
}

void serviceUpload() {
    if (!up.active) return;
    if (up.receiving) {
        if (millis() - up.lastDataMs > UPLOAD_STALL_MS)
            cancelUpload(otp_link_ResultCode_RESULT_TIMEOUT, "no data for 20 s", up.epoch == S.epoch);
        return;
    }
    if (up.commitDue && commitAllowed()) commitUpload();
}

// ---------------------------------------------------------------------------
// Downloads (device -> phone)

void endDownload() {
    if (dl.f) {
        sdLock();
        dl.f.close();
        sdUnlock();
    }
    dl = Download();
}

void cancelDownload(otp_link_ResultCode reason, const char* why, bool tellPhone) {
    diag::log("link: download %u (%s) cancelled: %s (%s) at %u/%u", (unsigned)dl.xfer, dl.label,
              resultName(reason), why, (unsigned)dl.next, (unsigned)dl.size);
    if (tellPhone) sendTransferCancel(dl.xfer, reason);
    endDownload();
}

void onDownload(uint32_t req, const otp_link_DownloadRequest& d) {
    char path[96];
    otp_link_ResultCode rc = otp_link_ResultCode_RESULT_OK;
    if (dl.active) {
        rc = otp_link_ResultCode_RESULT_BUSY;
    } else {
        switch (d.source) {
        case otp_link_Source_SOURCE_RIDE:
            if (!safeName(d.name)) rc = otp_link_ResultCode_RESULT_BAD_REQUEST;
            else rc = rideCardState();
            snprintf(path, sizeof(path), RIDE_DIR "/%s", d.name);
            break;
        case otp_link_Source_SOURCE_LOG:
            if (!diag::validLogName(d.name)) rc = otp_link_ResultCode_RESULT_BAD_REQUEST;
            else rc = rideCardState();
            snprintf(path, sizeof(path), "/logs/%s", d.name);
            break;
        case otp_link_Source_SOURCE_LOG_TODAY:
            rc = rideCardState();
            if (rc == otp_link_ResultCode_RESULT_OK) diag::flushToSD();   // newest lines too
            snprintf(path, sizeof(path), "%s", diag::logPath());
            break;
        case otp_link_Source_SOURCE_WORKOUT:
            if (!safeName(d.name)) rc = otp_link_ResultCode_RESULT_BAD_REQUEST;
            else rc = cardState();
            snprintf(path, sizeof(path), "/workouts/%s", d.name);
            break;
        default:
            rc = otp_link_ResultCode_RESULT_UNSUPPORTED;
            break;
        }
        if (d.source == otp_link_Source_SOURCE_LOG && rc == otp_link_ResultCode_RESULT_OK)
            diag::flushToSD();
    }
    File f;
    if (rc == otp_link_ResultCode_RESULT_OK) {
        sdLock();
        f = SD.open(path, FILE_READ);
        sdUnlock();
        if (!f) rc = otp_link_ResultCode_RESULT_NOT_FOUND;
    }
    uint32_t size = 0;
    if (rc == otp_link_ResultCode_RESULT_OK) {
        size = (uint32_t)f.size();
    }
    // A resume point past the end means the file changed under the phone:
    // start over (DownloadBegin.offset 0 tells it to drop what it has).
    const uint32_t start = d.offset > size ? 0 : d.offset;
    if (rc != otp_link_ResultCode_RESULT_OK) {
        if (f) { sdLock(); f.close(); sdUnlock(); }
        diag::log("link: download %u (source %d '%s') refused: %s", (unsigned)d.xfer,
                  (int)d.source, d.name, resultName(rc));
        sendResult(req, rc);
        return;
    }
    // TransferDone's CRC covers the whole file: on a resume, hash what the
    // phone already has first.
    uint32_t crc = 0;
    bool readOk = true;
    for (uint32_t off = 0; off < start && readOk;) {
        const uint32_t n = start - off < 4096 ? start - off : 4096;
        sdLock();
        readOk = f.read(ioBuf, n) == (int)n;
        sdUnlock();
        crc = link_crc32(crc, ioBuf, n);
        off += n;
        vTaskDelay(1);
    }
    if (!readOk) {
        sdLock(); f.close(); sdUnlock();
        sendResult(req, otp_link_ResultCode_RESULT_IO_ERROR);
        return;
    }
    dl = Download();
    dl.active = true;
    dl.xfer = d.xfer;
    dl.size = size;
    dl.next = dl.acked = dl.startOffset = start;
    dl.window = d.window ? (d.window < 1024 ? 1024 : d.window > 65536 ? 65536 : d.window)
                         : DOWNLOAD_WINDOW;
    dl.crc = crc;
    dl.startMs = dl.lastAckMs = millis();
    copyStr(dl.label, sizeof(dl.label), d.source == otp_link_Source_SOURCE_LOG_TODAY ? "today's log" : d.name);
    dl.f = f;
    ble_shared::noteBulk();
    diag::log("link: download %u begin: %s, %u bytes from %u, window %u", (unsigned)dl.xfer,
              dl.label, (unsigned)size, (unsigned)start, (unsigned)dl.window);
    otp_link_DownloadBegin& b = msg(req, otp_link_DeviceMsg_download_begin_tag).body.download_begin;
    b.xfer = dl.xfer;
    b.size = dl.size;
    b.offset = dl.next;
    send();
}

// Payload bytes per Chunk so the whole DeviceMsg fits one fragment
// (docs/ble-link.md: MTU - 3 - 1 - envelope and chunk fields, exactly).
size_t chunkPayload(uint32_t xfer, uint32_t offset) {
    const uint16_t mtu = ble_shared::mtu();
    const size_t frag = mtu > 3 ? (size_t)mtu - 3 : 20;
    const size_t overhead = 1 /*frag header*/ + 4 /*body tag + len*/ +
                            1 + varintLen(xfer) + 1 + varintLen(offset) + 1 + 2 /*data*/;
    size_t n = frag > overhead + 16 ? frag - overhead : 16;
    if (n > 512) n = 512;   // link.options Chunk.data
    return n;
}

bool downloadCanSend() {
    return dl.active && (dl.next >= dl.size || dl.next - dl.acked < dl.window);
}

// The last chunk went: the whole file's size and CRC, and done.
void finishDownload() {
    diag::log("link: download %u done: %s, %u bytes in %.1fs", (unsigned)dl.xfer, dl.label,
              (unsigned)(dl.size - dl.startOffset), (millis() - dl.startMs) / 1000.0f);
    sendTransferDone(0, dl.xfer, otp_link_ResultCode_RESULT_OK, nullptr, dl.size, dl.crc);
    endDownload();
}

void serviceDownload() {
    if (!dl.active) return;
    if (dl.next >= dl.size) {   // an empty file, or resumed at the end
        finishDownload();
        return;
    }
    if (usb_storage::hostActive()) {   // a computer took the card mid-stream
        cancelDownload(otp_link_ResultCode_RESULT_NO_SD, "USB host has the card", true);
        return;
    }
    if (dl.next - dl.acked >= dl.window) {
        if (millis() - dl.lastAckMs > DOWNLOAD_ACK_TIMEOUT_MS)
            cancelDownload(otp_link_ResultCode_RESULT_TIMEOUT, "no ack for 10 s", true);
        return;
    }
    size_t n = chunkPayload(dl.xfer, dl.next);
    if (n > dl.size - dl.next) n = dl.size - dl.next;
    otp_link_Chunk& c = msg(0, otp_link_DeviceMsg_chunk_tag).body.chunk;
    c.xfer = dl.xfer;
    c.offset = dl.next;
    sdLock();
    const int got = dl.f.read(c.data.bytes, n);
    sdUnlock();
    if (got != (int)n) {
        cancelDownload(otp_link_ResultCode_RESULT_IO_ERROR, "SD read failed", true);
        return;
    }
    c.data.size = (pb_size_t)n;
    dl.crc = link_crc32(dl.crc, c.data.bytes, n);
    ble_shared::noteBulk();
    if (!send(true)) {
        cancelDownload(otp_link_ResultCode_RESULT_IO_ERROR, "notify failed", false);
        return;
    }
    dl.next += n;
    if (dl.next >= dl.size) finishDownload();
}

void onTransferAck(const otp_link_TransferAck& a) {
    if (!dl.active || a.xfer != dl.xfer) return;
    if (a.received > dl.acked && a.received <= dl.next) dl.acked = a.received;
    dl.lastAckMs = millis();
}

void onTransferCancel(const otp_link_TransferCancel& c) {
    if (up.active && up.receiving && c.xfer == up.xfer)
        cancelUpload(c.reason, "by the phone", false);
    else if (dl.active && c.xfer == dl.xfer)
        cancelDownload(c.reason, "by the phone", false);
}

// ---------------------------------------------------------------------------
// Session

constexpr otp_link_Feature kFeatures[] = {
    otp_link_Feature_FEATURE_SETTINGS,
    otp_link_Feature_FEATURE_WHEEL_SIZE,
    otp_link_Feature_FEATURE_LIVE,
    otp_link_Feature_FEATURE_STOPWATCH,
    otp_link_Feature_FEATURE_STOPWATCH_LAPS,
    otp_link_Feature_FEATURE_ROUTES,
    otp_link_Feature_FEATURE_NAVIGATION,
    otp_link_Feature_FEATURE_PHONE_LOCATION,
    otp_link_Feature_FEATURE_RIDES,
    otp_link_Feature_FEATURE_LOGS,
    otp_link_Feature_FEATURE_FIRMWARE_UPDATE,
    otp_link_Feature_FEATURE_SENSORS,
    otp_link_Feature_FEATURE_MAPS,
    otp_link_Feature_FEATURE_MAP_TILES,
    otp_link_Feature_FEATURE_MAP_TILE_VERSIONS,
    otp_link_Feature_FEATURE_MAP_POI,
    otp_link_Feature_FEATURE_MAP_LAYERS,
    otp_link_Feature_FEATURE_DASH,
    otp_link_Feature_FEATURE_MESH,
    otp_link_Feature_FEATURE_MESH_RADIO_CONFIG,
    otp_link_Feature_FEATURE_MEDIA_META,
    otp_link_Feature_FEATURE_MEDIA_ART,
    otp_link_Feature_FEATURE_MEDIA_ART_TONE,
    otp_link_Feature_FEATURE_MEDIA_TRACK,
    otp_link_Feature_FEATURE_MEDIA_REMOTE,
    otp_link_Feature_FEATURE_WORKOUTS,
    otp_link_Feature_FEATURE_AGNSS,
    otp_link_Feature_FEATURE_UNPAIR,
};

// Transfers, scans and queued requests of a session that ended (or an app
// that said Hello again). A verified upload still waiting for the card is
// kept: it commits, it just has nobody to tell.
void dropSessionWork(const char* why) {
    if (up.active && up.receiving) cancelUpload(otp_link_ResultCode_RESULT_CANCELLED, why, false);
    if (dl.active) cancelDownload(otp_link_ResultCode_RESULT_CANCELLED, why, false);
    if (S.sensorsStreaming) ble_sensors::setScanAlways(false);
    deferredCount = 0;
}

// Link task: notice a new connection epoch.
void syncEpoch() {
    const uint32_t e = connEpoch.load();
    if (e == S.epoch) return;
    if (S.hello) diag::log("link: session ended");
    dropSessionWork("disconnected");
    S = Session();
    S.epoch = e;
    pushMask = 0;
    pendingMediaCmd = 0;
}

void onHello(uint32_t req, const otp_link_Hello& h) {
    const char* platform = h.platform == otp_link_Platform_PLATFORM_IOS ? "iOS"
                         : h.platform == otp_link_Platform_PLATFORM_ANDROID ? "Android" : "?";
    if (S.hello) dropSessionWork("hello again");
    // A second Hello on this connection (the app restarted under iOS state
    // restoration) starts the session over; the TX counter runs on.
    const uint32_t epoch = S.epoch;
    const uint8_t txCounter = S.txCounter;
    S = Session();
    S.epoch = epoch;
    S.txCounter = txCounter;
    for (pb_size_t i = 0; i < h.features_count; ++i)
        if (h.features[i] == otp_link_Feature_FEATURE_MEDIA_COMMAND_HANDLER) S.mediaHandler = true;
    S.hello = true;
    helloEpoch = epoch;
    diag::log("link: hello from %s app %s (proto %u, %u feature(s)), MTU %u", platform,
              h.app_version, (unsigned)h.proto, (unsigned)h.features_count,
              (unsigned)ble_shared::mtu());
    if (h.unix_ms) ble_shared::applyPhoneTime((time_t)(h.unix_ms / 1000));

    otp_link_DeviceHello& d = msg(req, otp_link_DeviceMsg_hello_tag).body.hello;
    d.proto = otp_link_ProtoVersion_PROTO_VERSION_CURRENT;
    copyStr(d.firmware, sizeof(d.firmware), FIRMWARE_VERSION);
    for (otp_link_Feature f : kFeatures) d.features[d.features_count++] = f;
    d.max_message = LINK_MAX_MESSAGE;
    d.mtu = ble_shared::mtu();
    d.dash_max_pages = DASH_MAX_PAGES - 1;   // content pages; the map page is extra
    d.dash_max_text = sizeof(otp_link_DashConfig::text) - 1;
    d.route_max_bytes = ROUTE_MAX;
    d.workout_max_bytes = WORKOUT_MAX;
    d.max_maneuvers = routes::MAX_MANEUVERS;
    if (!send()) return;

    // The initial state, unasked.
    sendSettings(0);
    S.dashVerSeen = dash_config::version();
    sendDash(0);
    sendMapLayers(0);
    S.workoutVerSeen = workout_service::version();
    sendWorkoutStatus(0);
    const uint32_t tv = media::amsTrackVersion();
    if (tv != 0) {
        S.trackVerSent = tv;
        sendMediaTrack(0);
    }
    S.topics = 1u << otp_link_LiveTopic_LIVE_TOPIC_RIDE;
    S.mediaVerSeen = media::version();
    S.liveKick = true;
}

void onLiveSubscribe(uint32_t req, const otp_link_LiveSubscribe& l) {
    uint32_t t = 0;
    for (pb_size_t i = 0; i < l.topics_count; ++i)
        if (l.topics[i] > otp_link_LiveTopic_LIVE_TOPIC_NONE &&
            l.topics[i] <= otp_link_LiveTopic_LIVE_TOPIC_MUSIC)
            t |= 1u << l.topics[i];
    if (t != S.topics) {
        static const char* const kNames[] = {"", "ride", "stopwatch", "view", "position", "radar", "music"};
        char names[64] = "";
        for (int i = 1; i <= 6; ++i)
            if (t & (1u << i)) {
                if (names[0]) strlcat(names, ",", sizeof(names));
                strlcat(names, kNames[i], sizeof(names));
            }
        diag::log("link: live topics %s", names[0] ? names : "none");
    }
    S.topics = t;
    S.liveKick = true;
    S.mediaVerSeen = media::version();
    sendResult(req, otp_link_ResultCode_RESULT_OK);
}

void onSetSettings(uint32_t req, const otp_link_Settings& s) {
    ble_shared::SettingsPatch p;
    if (s.has_ftp_w) { p.hasFtp = true; p.ftpW = s.ftp_w; }
    if (s.has_tz_min) { p.hasTz = true; p.tzMin = s.tz_min; }
    if (s.has_use_miles) { p.hasMiles = true; p.useMiles = s.use_miles; }
    if (s.has_backlight) { p.hasBacklight = true; p.backlight = s.backlight > 255 ? 255 : (int)s.backlight; }
    if (s.has_clock_24h) { p.hasClock24 = true; p.clock24h = s.clock_24h; }
    if (s.has_usb_drive) { p.hasUsbDrive = true; p.usbDrive = s.usb_drive; }
    if (s.has_wheel_mm) { p.hasWheel = true; p.wheelMm = s.wheel_mm > 100000 ? 100000 : (int)s.wheel_mm; }
    ble_shared::applySettings(p);
    sendSettings(req);   // the values now in force (clamped / refused ones show)
}

void onLocation(const otp_link_PhoneLocation& l) {
    ble_shared::PhoneFix f;
    f.latE7 = l.lat_e7;
    f.lonE7 = l.lon_e7;
    if (l.has_utc) { f.haveUtc = true; f.utc = l.utc; }
    if (l.has_alt_m) {
        f.haveAlt = true;
        f.altM = (int16_t)(l.alt_m < -32768 ? -32768 : l.alt_m > 32767 ? 32767 : l.alt_m);
    }
    if (l.has_accuracy_m) f.accM = (int16_t)(l.accuracy_m > 32767 ? 32767 : l.accuracy_m);
    if (l.has_speed_cms) f.speedMs = l.speed_cms / 100.0f;
    if (l.has_course_cdeg && l.course_cdeg < 36000) f.courseDeg = l.course_cdeg / 100.0f;
    ble_shared::applyPhoneLocation(f);   // not bulk: a 1-3 s fix must not pin the interval
}

void onSensors(uint32_t req, const otp_link_SensorCommand& c) {
    switch (c.op) {
    case otp_link_SensorOp_SENSOR_OP_SCAN_START:
        ble_sensors::setScanAlways(true);
        ble_sensors::noteActivity();
        S.sensorsStreaming = true;
        sendResult(req, otp_link_ResultCode_RESULT_OK);
        sendSensorList(0);
        return;
    case otp_link_SensorOp_SENSOR_OP_SCAN_STOP:
        ble_sensors::setScanAlways(false);
        S.sensorsStreaming = false;
        sendResult(req, otp_link_ResultCode_RESULT_OK);
        return;
    case otp_link_SensorOp_SENSOR_OP_PAIR:
    case otp_link_SensorOp_SENSOR_OP_FORGET:
        if (!c.address[0]) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "address");
            return;
        }
        if (c.op == otp_link_SensorOp_SENSOR_OP_PAIR) ble_sensors::pairCandidate(c.address);
        else ble_sensors::forget(c.address);
        sendSensorList(req);
        return;
    case otp_link_SensorOp_SENSOR_OP_LIST:
        sendSensorList(req);
        return;
    default:
        sendResult(req, otp_link_ResultCode_RESULT_UNSUPPORTED);
        return;
    }
}

void onWorkout(uint32_t req, const otp_link_WorkoutCommand& w) {
    switch (w.op) {
    case otp_link_WorkoutOp_WORKOUT_OP_START: workout_service::start(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_PAUSE: workout_service::pause(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_RESUME: workout_service::resume(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_STOP: workout_service::stop(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_SKIP: workout_service::skip(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_JUMP:
        workout_service::jumpToSeg(w.segment > 255 ? 255 : (int)w.segment);
        break;
    case otp_link_WorkoutOp_WORKOUT_OP_PAUSE_EACH:
        settings::setWorkoutPauseEachBlock(w.pause_each);
        break;
    case otp_link_WorkoutOp_WORKOUT_OP_UNLOAD: workout_service::unload(); break;
    case otp_link_WorkoutOp_WORKOUT_OP_STATUS: break;
    case otp_link_WorkoutOp_WORKOUT_OP_LOAD: {
        if (!safeName(w.name)) {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST, "name");
            return;
        }
        if (usb_storage::hostActive()) {
            sendResult(req, otp_link_ResultCode_RESULT_NO_SD);
            return;
        }
        const char* reason = "";
        if (workout_service::load(w.name, &reason)) sendResult(req, otp_link_ResultCode_RESULT_OK);
        else sendResult(req, otp_link_ResultCode_RESULT_PARSE_ERROR, reason);
        return;   // the version change pushes the new status
    }
    default:
        sendResult(req, otp_link_ResultCode_RESULT_UNSUPPORTED);
        return;
    }
    sendWorkoutStatus(req);
}

void onMediaMeta(uint32_t req, const otp_link_MediaMeta& m) {
    // Ignored inside media:: while AMS reports (it sees every player).
    media::setMeta(m.playing, (uint16_t)(m.position_s > 0xFFFF ? 0xFFFF : m.position_s),
                   (uint16_t)(m.duration_s > 0xFFFF ? 0xFFFF : m.duration_s), m.title, m.artist,
                   m.album);
    sendResult(req, otp_link_ResultCode_RESULT_OK);
}

void onSetDash(uint32_t req, const otp_link_DashConfig& d) {
    // Applied when the card is free, like the legacy commit; one at a time.
    for (int i = 0; i < deferredCount; ++i)
        if (deferred[i].kind == otp_link_ListKind_LIST_NONE) {
            sendResult(req, otp_link_ResultCode_RESULT_BUSY, "a layout is waiting for the card");
            return;
        }
    copyStr(dashPending, sizeof(otp_link_DashConfig::text), d.text);
    if (!defer(req, otp_link_ListKind_LIST_NONE)) sendResult(req, otp_link_ResultCode_RESULT_BUSY);
}

void applyPendingDash(uint32_t req) {
    const uint32_t before = dash_config::version();
    const char* reason = nullptr;
    const bool ok = ble_shared::applyDash(dashPending, &reason);
    S.dashVerSeen = dash_config::version();
    if (ok || S.dashVerSeen != before) {
        // Applied (a failed SD save still changes what the device shows).
        if (!ok) diag::log("dash: app layout applied but not saved (%s)", reason ? reason : "?");
        sendDash(req);
    } else {
        diag::log("dash: app layout rejected (%s)", reason ? reason : "?");
        sendResult(req, otp_link_ResultCode_RESULT_PARSE_ERROR, reason);
    }
}

void serviceDeferred() {
    if (!deferredCount || !ble_shared::sdFree()) return;
    const Deferred d = deferred[0];
    for (int i = 1; i < deferredCount; ++i) deferred[i - 1] = deferred[i];
    --deferredCount;
    switch (d.kind) {
    case otp_link_ListKind_LIST_NONE: applyPendingDash(d.req); break;
    case otp_link_ListKind_LIST_MAPS: sendMapCoverage(d.req); break;
    default: sendTileList(d.req, d.kind); break;
    }
}

void dispatch(const otp_link_PhoneMsg& m) {
    const uint32_t req = m.req;
    if (m.which_body == otp_link_PhoneMsg_hello_tag) {
        onHello(req, m.body.hello);
        return;
    }
    // Until Hello the device sends nothing on the link (docs/ble-link.md).
    if (!S.hello) return;
    switch (m.which_body) {
    case otp_link_PhoneMsg_unpair_tag:
        diag::log("link: unpair requested by the paired phone");
        ble_server::requestUnpair();   // no reply: the phone sees the disconnect
        break;
    case otp_link_PhoneMsg_get_settings_tag: sendSettings(req); break;
    case otp_link_PhoneMsg_set_settings_tag: onSetSettings(req, m.body.set_settings); break;
    case otp_link_PhoneMsg_live_subscribe_tag: onLiveSubscribe(req, m.body.live_subscribe); break;
    case otp_link_PhoneMsg_stopwatch_tag:
        // No reply: the next LiveFrame's StopwatchState is the answer.
        if (m.body.stopwatch.op >= otp_link_StopwatchOp_STOPWATCH_OP_TOGGLE &&
            m.body.stopwatch.op <= otp_link_StopwatchOp_STOPWATCH_OP_LAP)
            ble_shared::queueStopwatch((uint8_t)m.body.stopwatch.op);
        break;
    case otp_link_PhoneMsg_upload_begin_tag: onUploadBegin(req, m.body.upload_begin); break;
    case otp_link_PhoneMsg_chunk_tag: onChunk(m.body.chunk); break;
    case otp_link_PhoneMsg_upload_end_tag: onUploadEnd(req, m.body.upload_end); break;
    case otp_link_PhoneMsg_download_tag: onDownload(req, m.body.download); break;
    case otp_link_PhoneMsg_transfer_ack_tag: onTransferAck(m.body.transfer_ack); break;
    case otp_link_PhoneMsg_transfer_cancel_tag: onTransferCancel(m.body.transfer_cancel); break;
    case otp_link_PhoneMsg_list_tag: onList(req, m.body.list.kind); break;
    case otp_link_PhoneMsg_delete_file_tag: onDelete(req, m.body.delete_file); break;
    case otp_link_PhoneMsg_location_tag: onLocation(m.body.location); break;
    case otp_link_PhoneMsg_maneuvers_tag:
        for (pb_size_t i = 0; i < m.body.maneuvers.items_count; ++i) {
            const otp_link_Maneuver& mv = m.body.maneuvers.items[i];
            routes::addManeuver(mv.lat_e7 / 1e7, mv.lon_e7 / 1e7, mv.text);
        }
        sendResult(req, otp_link_ResultCode_RESULT_OK);
        break;
    case otp_link_PhoneMsg_route_finish_tag:
        routes::finishManeuvers();   // raises "Start navigation?"
        diag::log("link: navigation ready: %d maneuvers, track %s", routes::maneuverCount(),
                  S.routeLoaded ? "loaded" : "NOT loaded");
        sendResult(req, S.routeLoaded ? otp_link_ResultCode_RESULT_OK
                                      : otp_link_ResultCode_RESULT_PARSE_ERROR);
        break;
    case otp_link_PhoneMsg_sensors_tag: onSensors(req, m.body.sensors); break;
    case otp_link_PhoneMsg_get_map_layers_tag: sendMapLayers(req); break;
    case otp_link_PhoneMsg_set_map_layers_tag:
        settings::setMapLayers((uint16_t)(m.body.set_map_layers.mask & 0xFFFF));
        sendMapLayers(req);   // what is now in force
        break;
    case otp_link_PhoneMsg_get_dash_tag: sendDash(req); break;
    case otp_link_PhoneMsg_set_dash_tag: onSetDash(req, m.body.set_dash); break;
    case otp_link_PhoneMsg_media_meta_tag: onMediaMeta(req, m.body.media_meta); break;
    case otp_link_PhoneMsg_media_clear_tag:
        media::clearFromApp();   // ignored while AMS reports
        sendResult(req, otp_link_ResultCode_RESULT_OK);
        break;
    case otp_link_PhoneMsg_media_command_tag:
        if (m.body.media_command.cmd >= otp_link_MediaCmd_MEDIA_CMD_TOGGLE &&
            m.body.media_command.cmd <= otp_link_MediaCmd_MEDIA_CMD_VOLUME_DOWN) {
            // The legacy hand-off: the server task sends it on over AMS.
            ble_server::mediaCommand((unsigned char)m.body.media_command.cmd);
            sendResult(req, otp_link_ResultCode_RESULT_OK);
        } else {
            sendResult(req, otp_link_ResultCode_RESULT_BAD_REQUEST);
        }
        break;
    case otp_link_PhoneMsg_workout_tag: onWorkout(req, m.body.workout); break;
    case otp_link_PhoneMsg_mesh_tag: onMesh(req, m.body.mesh); break;
    case otp_link_PhoneMsg_agnss_info_tag: {
        const int k = gps_service::moduleKindCode();
        otp_link_AgnssInfo& a = msg(req, otp_link_DeviceMsg_agnss_info_tag).body.agnss_info;
        a.module = k == 1 ? otp_link_GnssModule_GNSS_MODULE_CASIC
                 : k == 2 ? otp_link_GnssModule_GNSS_MODULE_UBLOX : otp_link_GnssModule_GNSS_MODULE_NONE;
        a.fix = g_state.snapshot().gpsFix;
        send();
        break;
    }
    default:
        sendResult(req, otp_link_ResultCode_RESULT_UNSUPPORTED);
        break;
    }
}

void handleItem(const uint8_t* p, size_t n) {
    syncEpoch();
    if (n < 4) return;
    uint32_t e;
    memcpy(&e, p, 4);
    if (e != S.epoch) return;   // queued by a connection that has ended
    memset(inMsg, 0, sizeof(*inMsg));
    pb_istream_t is = pb_istream_from_buffer(p + 4, n - 4);
    if (!pb_decode(&is, otp_link_PhoneMsg_fields, inMsg)) {
        diag::log("link: decode failed (%u bytes, req %u): %s", (unsigned)(n - 4),
                  (unsigned)inMsg->req, PB_GET_ERROR(&is));
        if (S.hello) sendResult(inMsg->req, otp_link_ResultCode_RESULT_UNSUPPORTED, "undecodable");
        return;
    }
    dispatch(*inMsg);
}

// ---------------------------------------------------------------------------
// Pushes and periodic work (link task, every pass)

void servicePushes() {
    const uint32_t p = pushMask.exchange(0);
    const uint8_t mc = pendingMediaCmd.exchange(0);
    if (!S.hello) return;
    if (p & phone_link::PUSH_SETTINGS) sendSettings(0);
    if (p & phone_link::PUSH_MAP_LAYERS) sendMapLayers(0);
    if (p & phone_link::PUSH_MESH_CHANGED) {
        msg(0, otp_link_DeviceMsg_mesh_changed_tag);
        send();
    }
    if (mc && S.mediaHandler) {
        msg(0, otp_link_DeviceMsg_media_command_tag).body.media_command.cmd = (otp_link_MediaCmd)mc;
        send();
    }
    const uint32_t now = millis();
    // Layout changed by someone else (a legacy write): the app's editor follows.
    if (dash_config::version() != S.dashVerSeen) {
        S.dashVerSeen = dash_config::version();
        sendDash(0);
    }
    // Workout: on load/unload/commands from the device, and 1 Hz while running.
    const uint32_t wv = workout_service::version();
    if (wv != S.workoutVerSeen ||
        (workout_service::running() && now - S.lastWorkoutPushMs > 1000)) {
        S.workoutVerSeen = wv;
        sendWorkoutStatus(0);
    }
    // AMS delivers title, artist and album separately; wait for 300 ms of
    // quiet so the app gets the whole track at once.
    const uint32_t tv = media::amsTrackVersion();
    if (tv != 0 && tv != S.trackVerSent && now - media::amsTrackChangedMs() >= 300) {
        S.trackVerSent = tv;
        sendMediaTrack(0);
    }
    if (S.sensorsStreaming && now - S.lastSensorPushMs > SENSOR_PUSH_MS) sendSensorList(0);
}

void serviceLive() {
    const uint8_t changes = ble_shared::takeLiveViewChanges();
    if (!S.hello || !S.topics) return;
    const uint32_t t = S.topics;
    bool changed =
        ((changes & ble_shared::LIVE_CHANGED_VIEW) && (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_VIEW))) ||
        ((changes & ble_shared::LIVE_CHANGED_STOPWATCH) &&
         (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_STOPWATCH)));
    if (t & (1u << otp_link_LiveTopic_LIVE_TOPIC_MUSIC)) {
        // A play/pause, seek or track change goes out at once, so the app's
        // bar stops (or jumps) with the player.
        const uint32_t mv = media::version();
        if (mv != S.mediaVerSeen) {
            S.mediaVerSeen = mv;
            changed = true;
        }
    }
    const uint32_t now = millis();
    if (!S.liveKick && !changed && now - S.lastLiveMs < LIVE_PERIOD_MS) return;
    S.liveKick = false;
    S.lastLiveMs = now;
    sendLiveFrame();   // not bulk: 1 Hz frames ride the relaxed interval
}

// ---------------------------------------------------------------------------
// NimBLE callbacks (host task)

void noteRxDrop(link_drop_t why) {
    // Rate-limited: a confused phone can send a lot of these.
    static uint32_t lastMs = 0;
    const uint32_t n = ++rxFrameDrops;
    if (lastMs == 0 || millis() - lastMs > 10000) {
        lastMs = millis();
        diag::log("link: rx fragment dropped: %s (%u so far)", link_drop_name(why), (unsigned)n);
    }
}

class RxCb : public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* c, NimBLEConnInfo& info) override {
        // Admitted phone only (the characteristic is WRITE_AUTHEN too).
        if (!ble_shared::phoneAdmitted() || info.getConnHandle() != ble_shared::phoneHandle()) return;
        if (!rxRing || !rxAsmBuf) return;
        const NimBLEAttValue v = c->getValue();
        link_drop_t why = LINK_DROP_NONE;
        const link_rx_t r = link_reasm_feed(&rxAsm, v.data(), v.size(), &why);
        if (why != LINK_DROP_NONE && why != LINK_DROP_EMPTY) noteRxDrop(why);
        if (r != LINK_RX_COMPLETE) return;
        // Whole message: hand it to the link task, tagged with this session.
        void* slot = nullptr;
        if (xRingbufferSendAcquire(rxRing, &slot, rxAsm.len + 4, 0) != pdTRUE || !slot) {
            static uint32_t lastMs = 0;
            const uint32_t n = ++rxRingDrops;
            if (lastMs == 0 || millis() - lastMs > 10000) {
                lastMs = millis();
                diag::log("link: rx queue full, message dropped (%u so far)", (unsigned)n);
            }
            return;
        }
        const uint32_t e = connEpoch.load();
        memcpy(slot, &e, 4);
        memcpy((uint8_t*)slot + 4, rxAsm.buf, rxAsm.len);
        xRingbufferSendComplete(rxRing, slot);
    }
};

class TxCb : public NimBLECharacteristicCallbacks {
    void onSubscribe(NimBLECharacteristic*, NimBLEConnInfo&, uint16_t subValue) override {
        diag::log("link: TX notifications %s", (subValue & 0x0001) ? "on" : "off");
    }
};

}  // namespace

namespace phone_link {

void begin(NimBLEServer* server, uint32_t rxProperties) {
    rxAsmBuf = (uint8_t*)heap_caps_malloc(LINK_MAX_MESSAGE, MALLOC_CAP_SPIRAM);
    link_reasm_init(&rxAsm, rxAsmBuf, rxAsmBuf ? LINK_MAX_MESSAGE : 0);
    uint8_t* ringStore = (uint8_t*)heap_caps_malloc(RX_RING_BYTES, MALLOC_CAP_SPIRAM);
    if (ringStore)
        rxRing = xRingbufferCreateStatic(RX_RING_BYTES, RINGBUF_TYPE_NOSPLIT, ringStore,
                                         &rxRingStruct);
    inMsg = (otp_link_PhoneMsg*)heap_caps_malloc(sizeof(otp_link_PhoneMsg), MALLOC_CAP_SPIRAM);
    outMsg = (otp_link_DeviceMsg*)heap_caps_malloc(sizeof(otp_link_DeviceMsg), MALLOC_CAP_SPIRAM);
    txBuf = (uint8_t*)heap_caps_malloc(otp_link_DeviceMsg_size, MALLOC_CAP_SPIRAM);
    ioBuf = (uint8_t*)heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    listBuf = (otp_link_FileEntry*)heap_caps_malloc(sizeof(otp_link_FileEntry) * LIST_MAX,
                                                    MALLOC_CAP_SPIRAM);
    dashPending = (char*)heap_caps_malloc(sizeof(otp_link_DashConfig::text), MALLOC_CAP_SPIRAM);
    if (!rxAsmBuf || !rxRing || !inMsg || !outMsg || !txBuf || !ioBuf || !listBuf || !dashPending) {
        diag::log("link: out of PSRAM — the phone link is off");
        rxRing = nullptr;   // RX ignores everything; the service still exists
    }

    NimBLEService* svc = server->createService(SVC_LINK);
    NimBLECharacteristic* rx = svc->createCharacteristic(CHR_RX, rxProperties);
    rx->setCallbacks(new RxCb());
    txChr = svc->createCharacteristic(CHR_TX, NIMBLE_PROPERTY::NOTIFY);
    txChr->setCallbacks(new TxCb());
}

void onAdmitted() {
    link_reasm_reset(&rxAsm);
    connEpoch.fetch_add(1);
}

void onDisconnected() {
    link_reasm_reset(&rxAsm);
    connEpoch.fetch_add(1);
}

bool active() {
    const uint32_t e = helloEpoch.load();
    return e != 0 && e == connEpoch.load() && ble_shared::phoneAdmitted();
}

void push(uint32_t what) { pushMask.fetch_or(what); }

void pushMediaCommand(uint8_t cmd) { pendingMediaCmd = cmd; }

void task(void*) {
    for (;;) {
        if (!rxRing) {   // out of PSRAM at boot: nothing to run
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        syncEpoch();
        // Wait for a request, or not at all while a download has a window
        // open. 20 ms keeps live frames and pushes prompt otherwise.
        size_t n = 0;
        void* item = xRingbufferReceive(rxRing, &n, downloadCanSend() ? 0 : pdMS_TO_TICKS(20));
        while (item) {
            handleItem((const uint8_t*)item, n);
            vRingbufferReturnItem(rxRing, item);
            item = xRingbufferReceive(rxRing, &n, 0);
        }
        syncEpoch();
        // Control first (answers above, then pushes and live frames), then at
        // most one bulk chunk, then round again: a button press or a frame
        // never waits behind more than one chunk.
        servicePushes();
        serviceLive();
        serviceDeferred();
        serviceUpload();
        if (sessionLive()) serviceDownload();
        else if (dl.active) cancelDownload(otp_link_ResultCode_RESULT_CANCELLED, "session ended", false);
    }
}

}  // namespace phone_link
