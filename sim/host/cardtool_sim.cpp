// `otp-sim card-tool`: host-side helpers that build SD card contents with the
// firmware's own code, for sim/client/otpsim/cards.py.
//
//   otp-sim card-tool h3 S W N E
//       the res-6 H3 cells (map tiles) covering the box, one per line:
//       "<id> <s> <w> <n> <e>" (the cell's own bounding box)
//   otp-sim card-tool fit SDROOT < specs
//       finished rides written by FitWriter (fit_writer.cpp) through the sim's
//       card layer, one per stdin line:
//       "<card path> <start unix> <seconds> <lat> <lon> <bearing deg> <km/h> <hr bpm|0> <watts|0>"
#include <FS.h>
#include <SD.h>

#include "fit_writer.h"
#include "h3shim.h"
#include "sim.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

int h3Cells(int argc, char** argv) {
    if (argc < 4) { fprintf(stderr, "card-tool h3 S W N E\n"); return 1; }
    const double s = atof(argv[0]), w = atof(argv[1]), n = atof(argv[2]), e = atof(argv[3]);
    std::vector<uint64_t> cells(200000);
    const int k = h3_covering_cells(s, w, n, e, cells.data(), (int)cells.size());
    for (int i = 0; i < k; ++i) {
        char id[24];
        double cs, cw, cn, ce;
        h3_cell_id(cells[i], id, sizeof id);
        h3_cell_bbox(cells[i], &cs, &cw, &cn, &ce);
        printf("%s %.7f %.7f %.7f %.7f\n", id, cs, cw, cn, ce);
    }
    return 0;
}

int fitRides(int argc, char** argv) {
    if (argc < 1) { fprintf(stderr, "card-tool fit SDROOT < specs\n"); return 1; }
    sim::opts.sdDir = argv[0];
    sim::opts.dir = argv[0];   // no sd-removed marker here: the card is in
    char line[512];
    int made = 0;
    while (fgets(line, sizeof line, stdin)) {
        char path[256];
        long long start;
        int secs, hr, watts;
        double lat, lon, bearing, kmh;
        if (sscanf(line, "%255s %lld %d %lf %lf %lf %lf %d %d", path, &start, &secs, &lat, &lon, &bearing, &kmh,
                   &hr, &watts) != 9)
            continue;
        std::string dir(path);
        dir = dir.substr(0, dir.rfind('/'));
        if (!dir.empty()) SD.mkdir(dir.c_str());
        FitWriter fw;
        if (!fw.begin(SD, path, (time_t)start)) { fprintf(stderr, "can't open %s\n", path); return 1; }
        const double ms = kmh / 3.6, r = bearing * M_PI / 180.0;
        double dist = 0, alt = 40, hrSum = 0, pSum = 0;
        for (int t = 0; t < secs; ++t) {
            // A gentle wiggle so the ride isn't a ruler line, and a rolling profile.
            const double v = ms * (0.85 + 0.15 * sin(t / 37.0));
            dist += v;
            const double d = dist, side = 40 * sin(t / 90.0);
            FitWriter::Record rec{};
            rec.utc = (time_t)(start + t);
            rec.latitudeDeg = lat + (d * cos(r) - side * sin(r)) / 111320.0;
            rec.longitudeDeg = lon + (d * sin(r) + side * cos(r)) / (111320.0 * cos(lat * M_PI / 180.0));
            alt = 40 + 30 * sin(t / 300.0);
            rec.altitudeM = (float)alt;
            rec.speedMs = (float)v;
            rec.distanceM = dist;
            rec.powerW = watts ? (uint16_t)(watts + 30 * sin(t / 11.0)) : FitWriter::INVALID_U16;
            rec.heartRate = hr ? (uint8_t)(hr + 8 * sin(t / 60.0)) : FitWriter::INVALID_U8;
            rec.cadence = watts ? (uint8_t)(85 + 5 * sin(t / 13.0)) : FitWriter::INVALID_U8;
            hrSum += rec.heartRate;
            pSum += watts ? rec.powerW : 0;
            fw.writeRecord(rec);
        }
        FitWriter::Summary sum{};
        sum.movingS = (uint32_t)secs;
        sum.avgSpeedKmh = secs ? (float)(dist / secs * 3.6) : 0;
        sum.avgPowerW = watts && secs ? (uint16_t)(pSum / secs) : 0;
        sum.normPowerW = sum.avgPowerW;
        sum.avgHrBpm = hr && secs ? (uint8_t)(hrSum / secs) : 0;
        sum.ascentM = 60;
        if (!fw.finish((time_t)(start + secs), dist, (uint32_t)secs, sum)) {
            fprintf(stderr, "can't finish %s\n", path);
            return 1;
        }
        ++made;
    }
    printf("%d\n", made);
    return 0;
}

}  // namespace

int sim::cardTool(int argc, char** argv) {
    const std::string what = argc > 0 ? argv[0] : "";
    if (what == "h3") return h3Cells(argc - 1, argv + 1);
    if (what == "fit") return fitRides(argc - 1, argv + 1);
    fprintf(stderr, "otp-sim card-tool h3 S W N E | fit SDROOT < specs\n");
    return 1;
}
