// The map page, rendered by the FIRMWARE'S OWN renderer, compiled to WebAssembly.
//
// QEMU's esp32s3 PSRAM model is broken (github.com/espressif/qemu/issues/129),
// and the map projector needs ~700 KB of PSRAM scratch it therefore can't have,
// so the firmware running in QEMU shows "NO MAP HERE" on its map page. This
// module compiles the very same code — map_tiles::project + ui_render_map, the
// same rasteriser (epd_compat.cpp) — for the browser, where RAM is unbounded, and
// renders a pixel-identical map frame. emulator.js draws it whenever the device
// is on the map page, passing the same GPS position it feeds the firmware.
//
// The framebuffer this produces is byte-for-byte what the firmware would stream
// out UART1 (same 960x540 4bpp buffer, same draw calls), so the page decodes it
// with the identical drawFrame() path used for real firmware frames.
//
// Build: tools/emu/build-map-wasm.sh  ->  web/emulator/map_wasm.js + .wasm

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include <emscripten.h>

#include "epd_compat.h"
#include "map_view.h"
#include "map_tiles.h"
#include "ride_state.h"
#include "ui_render.h"

// map_view.cpp references these for the route-preview path; the emulator has no
// route loaded, so they are empty. (Same stubs tools/preview uses on the host.)
namespace routes {
int pointCount() { return 0; }
void point(int, double& lat, double& lon) { lat = lon = 0; }
const char* activeName() { return ""; }
}

static constexpr int NATIVE_W = 960, NATIVE_H = 540;
static uint8_t g_fb[NATIVE_W / 2 * NATIVE_H];   // 259200 B — the firmware's fb
static std::vector<uint8_t> g_blob;             // the embedded SF map (sf.ebm)
static bool g_loaded = false;

extern "C" {

// Load the embedded San Francisco map once. Returns 1 on success.
EMSCRIPTEN_KEEPALIVE
int map_init() {
    // Match the emulator firmware's panel rotation (epd_compat_emu.cpp) so this
    // framebuffer has the same orientation as the frames QEMU streams — the page
    // blits both through the identical path.
    epd_set_rotation(EPD_ROT_PORTRAIT);
    FILE* f = fopen("sf.ebm", "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 36) { fclose(f); return 0; }
    g_blob.resize((size_t)sz);
    size_t got = fread(g_blob.data(), 1, (size_t)sz, f);
    fclose(f);
    if (got != (size_t)sz) return 0;
    g_loaded = map_tiles::load(g_blob.data(), g_blob.size());
    return g_loaded ? 1 : 0;
}

EMSCRIPTEN_KEEPALIVE uint8_t* map_fb() { return g_fb; }
EMSCRIPTEN_KEEPALIVE int map_fb_len() { return (int)sizeof(g_fb); }

// Render the whole map page (map + status bar + data strip) for the given state,
// exactly as the device does. mpp = metres per pixel (zoom). trackUp rotates the
// world to the heading. The sensor/clock fields drive the chrome.
EMSCRIPTEN_KEEPALIVE
void map_render(double lat, double lon, float mpp, float courseDeg, int trackUp,
                int batteryPct, int hr, int pwr, int cad,
                int hasHr, int hasPwr, int hasCad,
                double speedKmh, double distanceM, unsigned elapsedS,
                double utc, int tzMin, int clock24h, int gpsFix, int useMiles) {
    memset(g_fb, 0xFF, sizeof(g_fb));
    if (!g_loaded) return;

    MapScreenData map = {};
    map.riderX = 270;
    map.riderY = 430;
    map.metersPerPixel = mpp;
    map.trackUp = trackUp != 0;
    map.northDeg = 0;
    map.headingDeg = trackUp ? 0.0f : courseDeg;
    const float rot = trackUp ? -courseDeg : 0.0f;
    map_tiles::project(lat, lon, mpp, 270, 430, rot, map);

    RideState s;
    s.gpsFix = gpsFix != 0;
    s.latitude = lat;
    s.longitude = lon;
    s.speedKmh = (float)speedKmh;
    s.courseDeg = courseDeg;
    s.heartRateBpm = (uint8_t)hr;
    s.powerW = (uint16_t)pwr;
    s.power3sW = (uint16_t)pwr;
    s.cadenceRpm = (uint8_t)cad;
    s.hrConnected = hasHr != 0;
    s.powerConnected = hasPwr != 0;
    s.cadenceConnected = hasCad != 0;
    s.distanceM = distanceM;
    s.elapsedS = elapsedS;
    s.batteryPercent = (uint8_t)batteryPct;
    s.timeValid = true;
    s.utc = (time_t)utc;
    s.tzMin = (int16_t)tzMin;
    s.clock24h = clock24h != 0;
    s.useMiles = useMiles != 0;

    ui_render_map(map, s, g_fb);
}

}  // extern "C"
