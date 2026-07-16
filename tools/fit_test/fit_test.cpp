// Host harness for FitWriter — compiles the real src/fit_writer.cpp against a
// thin FS shim so the crash/recovery paths can be exercised without hardware.
//
// Emits .fit files into tools/fit_test/out/ for a real parser to validate:
//   normal.fit    — ride closed cleanly via finish()
//   crashed.fit   — ride cut off mid-stream, then repaired in place
//   torn.fit      — cut off mid-record, then repaired in place
//   empty.fit     — cut off before the first GPS fix (prologue only)
//
// Build/run: tools/fit_test/run_fit_test.sh

#include "fit_writer.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

const char* OUT_DIR = "tools/fit_test/out";
fs::FS host(OUT_DIR);

// 2026-07-16 17:39:05 UTC — the peninsula ride from issue #5.
constexpr time_t RIDE_START = 1784223545;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) { printf("  FAIL: %s\n", what); failures++; }
}

std::string fullPath(const char* p) { return std::string(OUT_DIR) + p; }

// Roughly 20 km/h heading south down the peninsula from San Francisco.
FitWriter::Record makeRecord(int i, double& distanceM) {
    distanceM += 5.5;  // ~5.5 m per 1 Hz sample
    FitWriter::Record r;
    r.utc = RIDE_START + i;
    r.latitudeDeg = 37.7527 - i * 0.00005;
    r.longitudeDeg = -122.4365 + i * 0.00001;
    r.altitudeM = 30.0f + (i % 40);
    r.speedMs = 5.5f;
    r.distanceM = distanceM;
    r.powerW = 180 + (i % 30);
    r.heartRate = 140 + (i % 10);
    r.cadence = 85;
    return r;
}

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

uint32_t fileSize(const char* path) { return (uint32_t)readAll(path).size(); }

// Writes a complete ride, then produces `dst` as the first `keepBytes` of it
// with the header's data_size rolled back to `staleDataSize` — byte-for-byte
// what a watchdog reset leaves on the card: records on disk, but a data_size
// frozen at the last 15 s checkpoint and no lap/session/activity/CRC tail.
void simulateCrash(const char* dst, int records, int extraBytes,
                   uint32_t staleDataSize) {
    {
        FitWriter w;
        if (!w.begin(host, "/scratch.fit", RIDE_START)) {
            printf("FAIL: cannot open scratch\n");
            exit(1);
        }
        double dist = 0;
        for (int i = 0; i < records; ++i) w.writeRecord(makeRecord(i, dist));
        // One more whole record when a torn tail is wanted; the truncation
        // below keeps only the first `extraBytes` of it.
        if (extraBytes > 0) w.writeRecord(makeRecord(records, dist));
        // finish() only serves to close the handle here — everything it appends
        // lands past `keep` and is truncated away.
        w.finish(RIDE_START + records, dist, (uint32_t)records);
    }
    std::vector<uint8_t> buf = readAll("/scratch.fit");
    size_t keep = 106 + (size_t)records * 25 + (size_t)extraBytes;
    if (buf.size() < keep) { printf("FAIL: scratch too small\n"); exit(1); }
    buf.resize(keep);
    buf[4] = staleDataSize & 0xFF;
    buf[5] = (staleDataSize >> 8) & 0xFF;
    buf[6] = (staleDataSize >> 16) & 0xFF;
    buf[7] = (staleDataSize >> 24) & 0xFF;
    FILE* f = fopen(fullPath(dst).c_str(), "wb");
    fwrite(buf.data(), 1, buf.size(), f);
    fclose(f);
    remove(fullPath("/scratch.fit").c_str());
}

}  // namespace

int main() {
    printf("prologue = %u bytes, record = 25 bytes\n\n", (unsigned)106);

    // 1. A clean ride, closed properly.
    {
        FitWriter w;
        if (!w.begin(host, "/normal.fit", RIDE_START)) { printf("FAIL: open\n"); return 1; }
        double dist = 0;
        for (int i = 0; i < 345; ++i) w.writeRecord(makeRecord(i, dist));
        check(w.finish(RIDE_START + 345, dist, 345), "finish() succeeds");
        printf("normal.fit   %6u bytes  clean finish\n", fileSize("/normal.fit"));
    }

    // 2. The issue #5 ride: 345 whole samples on disk, data_size frozen at the
    //    last checkpoint (345 = 23 checkpoints of 15 records: 340 records in).
    simulateCrash("/crashed.fit", 345, 0, 106 - 12 + 340 * 25);
    printf("crashed.fit  %6u bytes  watchdog reset, never finished\n",
           fileSize("/crashed.fit"));
    check(fileSize("/crashed.fit") == 8731, "crashed size matches the 8731 in the log");
    {
        FitWriter::RepairResult r = FitWriter::repair(host, "/crashed.fit");
        printf("             -> %s: %d records, %.2f km, %lu s\n",
               r.status == FitWriter::RepairResult::REPAIRED ? "REPAIRED" : "FAILED",
               r.records, r.distanceM / 1000.0, (unsigned long)r.elapsedS);
        check(r.status == FitWriter::RepairResult::REPAIRED, "crashed ride repaired");
        check(r.records == 345, "all 345 records recovered");
        check(r.elapsedS == 344, "elapsed spans the record stream");
    }

    // 3. Reset mid-record: the torn tail must be dropped, not decoded.
    simulateCrash("/torn.fit", 100, 11, 0);
    {
        FitWriter::RepairResult r = FitWriter::repair(host, "/torn.fit");
        printf("torn.fit     %6u bytes  -> %s: %d records\n", fileSize("/torn.fit"),
               r.status == FitWriter::RepairResult::REPAIRED ? "REPAIRED" : "FAILED",
               r.records);
        check(r.status == FitWriter::RepairResult::REPAIRED, "torn ride repaired");
        check(r.records == 100, "torn trailing record dropped");
    }

    // 4. A ride that died before the first GPS fix — no samples to salvage.
    simulateCrash("/empty.fit", 0, 0, 0);
    {
        FitWriter::RepairResult r = FitWriter::repair(host, "/empty.fit");
        printf("empty.fit    %6u bytes  -> %s\n", fileSize("/empty.fit"),
               r.status == FitWriter::RepairResult::EMPTY ? "EMPTY" : "unexpected");
        check(fileSize("/empty.fit") == 106, "empty size matches the 106 in the log");
        check(r.status == FitWriter::RepairResult::EMPTY, "empty ride reported EMPTY");
    }

    // 5. Recovery runs every boot, so a finished ride must be left alone.
    {
        std::vector<uint8_t> before = readAll("/normal.fit");
        FitWriter::RepairResult r = FitWriter::repair(host, "/normal.fit");
        printf("normal.fit   -> %s\n",
               r.status == FitWriter::RepairResult::ALREADY_FINISHED
                   ? "ALREADY_FINISHED (skipped)" : "unexpected");
        check(r.status == FitWriter::RepairResult::ALREADY_FINISHED,
              "finished ride skipped");
        check(readAll("/normal.fit") == before, "finished ride left byte-identical");
    }

    // 6. Repair is idempotent — a repaired ride is now a finished ride.
    {
        FitWriter::RepairResult r = FitWriter::repair(host, "/crashed.fit");
        check(r.status == FitWriter::RepairResult::ALREADY_FINISHED,
              "repair is idempotent");
    }

    printf("\n%s\n", failures ? "HOST ASSERTIONS FAILED" : "all host assertions passed");
    return failures ? 1 : 0;
}
