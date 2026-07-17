// Reproduces issue #12: the phone app's ascent must match the device's
// end-ride summary. The device integrates ascent from the map DEM elevation
// only (3 m hysteresis) and deliberately ignores the noisy GPS altitude. The
// FIT record stream must therefore carry the same DEM elevation — and mark the
// altitude invalid when the DEM has no value — never the raw GPS altitude.
//
// This harness builds a ride where the DEM drops out for a stretch (a tunnel /
// tile gap) during which the GPS altitude sits ~45 m off the DEM. It compares:
//   * device ascent   — map DEM only, 3 m hysteresis (ride_recorder.cpp)
//   * app ascent (new) — FIT altitude = DEM, invalid where DEM missing
//   * app ascent (old) — FIT altitude = DEM, GPS altitude where DEM missing
// against a FIT decode that mirrors companion-ios/Sources/FITDecoder.swift.
//
// Build/run: tools/fit_test/run_ascent_test.sh

#include "fit_writer.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* OUT_DIR = "tools/fit_test/out";
fs::FS host(OUT_DIR);

constexpr time_t RIDE_START = 1784223545;

int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
    else     { printf("  ok:   %s\n", what); }
}

std::string fullPath(const char* p) { return std::string(OUT_DIR) + p; }

std::vector<uint8_t> readAll(const char* path) {
    FILE* f = fopen(fullPath(path).c_str(), "rb");
    if (!f) return {};
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> buf((size_t)n);
    if (n > 0 && fread(buf.data(), 1, (size_t)n, f) != (size_t)n) buf.clear();
    fclose(f);
    return buf;
}

// The device's ascent accumulation, verbatim from ride_recorder.cpp.
double deviceAscent(const std::vector<float>& demElev,
                    const std::vector<bool>& demValid) {
    double climbedM = 0;
    float base = 0;
    bool baseValid = false;
    for (size_t i = 0; i < demElev.size(); ++i) {
        if (!demValid[i]) continue;
        float elev = demElev[i];
        if (!baseValid) { base = elev; baseValid = true; }
        else if (elev > base + 3.0f) { climbedM += elev - base; base = elev; }
        else if (elev < base - 3.0f) { base = elev; }
    }
    return climbedM;
}

// The app's ascent, mirroring FITDecoder.swift: decode the record stream, skip
// altitudes encoded as 0xFFFF, integrate with the same 3 m hysteresis.
double appAscentFromFit(const char* path) {
    std::vector<uint8_t> b = readAll(path);
    if (b.size() < 14) { printf("  (no file %s)\n", path); return -1; }
    int headerSize = b[0];
    int dataSize = b[4] | (b[5] << 8) | (b[6] << 16) | (b[7] << 24);
    int end = headerSize + dataSize;
    if (dataSize == 0 || end > (int)b.size() || end < headerSize) end = (int)b.size();

    struct FieldDef { int num, size; };
    struct MsgDef { int global; std::vector<FieldDef> fields; };
    MsgDef defs[16];
    bool defSet[16] = {false};

    double ascent = 0;
    bool haveBase = false;
    double base = 0;
    int i = headerSize;
    while (i < end) {
        uint8_t rec = b[i++];
        if (rec & 0x40) {
            int local = rec & 0x0F;
            if (i + 5 > end) break;
            int global = b[i + 2] | (b[i + 3] << 8);
            int count = b[i + 4];
            i += 5;
            MsgDef d; d.global = global;
            for (int c = 0; c < count; ++c) {
                if (i + 3 > end) break;
                d.fields.push_back({b[i], b[i + 1]});
                i += 3;
            }
            defs[local] = d; defSet[local] = true;
        } else {
            int local = rec & 0x0F;
            if (!defSet[local]) break;
            const MsgDef& d = defs[local];
            int off = i;
            bool haveAlt = false; double alt = 0;
            for (const auto& f : d.fields) {
                if (off + f.size > end) break;
                if (d.global == 20 && f.num == 2) {
                    uint16_t v = b[off] | (b[off + 1] << 8);
                    if (v != 0xFFFF) { alt = (double)v / 5.0 - 500.0; haveAlt = true; }
                }
                off += f.size;
            }
            i = off;
            if (d.global == 20 && haveAlt) {
                if (haveBase) {
                    if (alt > base + 3) { ascent += alt - base; base = alt; }
                    else if (alt < base - 3) { base = alt; }
                } else { base = alt; haveBase = true; }
            }
        }
    }
    return ascent;
}

// Writes a ride to `path`. demValid[i]==false points get `invalidAlt` as the
// record altitude: NAN models the new firmware, a GPS altitude models the old.
void writeRide(const char* path, const std::vector<float>& demElev,
               const std::vector<bool>& demValid,
               const std::vector<float>& gpsAlt, bool useNanWhenInvalid) {
    FitWriter w;
    if (!w.begin(host, path, RIDE_START)) { printf("FAIL open %s\n", path); exit(1); }
    double dist = 0;
    for (size_t i = 0; i < demElev.size(); ++i) {
        dist += 5.5;
        FitWriter::Record r;
        r.utc = RIDE_START + (time_t)i;
        r.latitudeDeg = 37.7527 - i * 0.00005;
        r.longitudeDeg = -122.4365 + i * 0.00001;
        if (demValid[i])           r.altitudeM = demElev[i];
        else if (useNanWhenInvalid) r.altitudeM = NAN;
        else                        r.altitudeM = gpsAlt[i];
        r.speedMs = 5.5f;
        r.distanceM = dist;
        r.powerW = FitWriter::INVALID_U16;
        r.heartRate = FitWriter::INVALID_U8;
        r.cadence = FitWriter::INVALID_U8;
        w.writeRecord(r);
    }
    w.finish(RIDE_START + (time_t)demElev.size(), dist, (uint32_t)demElev.size());
}

}  // namespace

int main() {
    // A steady climb DEM: 100 m rising ~0.7 m/sample to ~170 m over 100 pts.
    // The DEM drops out for samples [30,50): a tile gap. During the gap the GPS
    // altitude sits ~45 m below the DEM (typical ellipsoid/geoid + noise), so
    // the old fallback injects a -45 m dip and a +45 m spike into the profile.
    const int N = 100;
    std::vector<float> dem(N), gps(N);
    std::vector<bool> valid(N, true);
    for (int i = 0; i < N; ++i) {
        dem[i] = 100.0f + 0.7f * i;
        gps[i] = dem[i] - 45.0f + (i % 5);   // offset + jitter
        if (i >= 30 && i < 50) valid[i] = false;
    }

    double devAscent = deviceAscent(dem, valid);

    writeRide("/ascent_new.fit", dem, valid, gps, /*useNanWhenInvalid=*/true);
    writeRide("/ascent_old.fit", dem, valid, gps, /*useNanWhenInvalid=*/false);

    double appNew = appAscentFromFit("/ascent_new.fit");
    double appOld = appAscentFromFit("/ascent_old.fit");

    printf("device summary ascent (map DEM only):      %6.1f m\n", devAscent);
    printf("app ascent, OLD firmware (GPS fallback):   %6.1f m\n", appOld);
    printf("app ascent, NEW firmware (invalid marker): %6.1f m\n\n", appNew);

    // The bug: the old GPS fallback makes the app disagree with the device.
    check(fabs(appOld - devAscent) > 20.0,
          "OLD firmware: app ascent diverges from device (reproduces the bug)");
    // The fix: invalid marker => app matches the device within the FIT
    // altitude quantization (0.2 m/LSB), not the ~50 m the old fallback caused.
    check(fabs(appNew - devAscent) < 1.0,
          "NEW firmware: app ascent matches device end-ride summary");

    printf("\n%s\n", failures ? "ASCENT TEST FAILED" : "ascent test passed");
    return failures ? 1 : 0;
}
