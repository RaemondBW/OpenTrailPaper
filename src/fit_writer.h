#pragma once

#include <Arduino.h>
#include <FS.h>

// Minimal Garmin FIT activity encoder — just enough for Strava/intervals.icu:
// file_id, event (timer start/stop), record stream, lap, session, activity.
//
// Layout: 12-byte legacy header (data_size patched on close), definition +
// data messages, trailing CRC-16 computed by re-reading the file at close.

class FitWriter {
public:
    // One data point, 1 Hz. Use the INVALID_* constants for missing values;
    // altitudeM is left out of the record (field marked invalid) when NAN.
    struct Record {
        time_t   utc;
        double   latitudeDeg;
        double   longitudeDeg;
        float    altitudeM;   // NAN => no elevation for this point
        float    speedMs;
        double   distanceM;
        uint16_t powerW;      // INVALID_U16 if absent
        uint8_t  heartRate;   // INVALID_U8 if absent
        uint8_t  cadence;     // INVALID_U8 if absent
    };

    static constexpr uint16_t INVALID_U16 = 0xFFFF;
    static constexpr uint8_t  INVALID_U8 = 0xFF;

    // Ride roll-up written into the session message so the app (and Strava etc.)
    // read exactly the numbers the device shows on its ride-complete screen —
    // including the map-DEM ascent, which can't be re-derived from the GPS track.
    struct Summary {
        uint32_t movingS;      // time moving (device "MOVING TIME")
        float    avgSpeedKmh;
        uint16_t avgPowerW;    // 0 => absent
        uint16_t normPowerW;   // 0 => absent
        uint8_t  avgHrBpm;     // 0 => absent
        float    ascentM;      // from the map DEM
    };

    // Opens the file and writes header, file_id and timer-start event.
    bool begin(fs::FS& fs, const char* path, time_t startUtc);

    void writeRecord(const Record& r);

    // Call periodically to push buffered records onto the card, so a crash
    // mid-ride loses seconds rather than minutes. This does NOT leave a
    // readable file behind: an activity is only valid once finish() has
    // appended the lap/session/activity messages and the trailing CRC. A ride
    // cut short by a reset is put back together by repair() on the next boot.
    void checkpoint();

    // Timer-stop event, lap, session, activity, then header/CRC fixup. The
    // summary is folded into the session message (empty for a repaired ride,
    // where only distance/time can be recovered).
    bool finish(time_t endUtc, double totalDistanceM, uint32_t timerS,
                const Summary& sum);

    bool isOpen() const { return (bool)file_; }

    struct RepairResult {
        enum Status {
            ALREADY_FINISHED,  // has a trailing CRC — left untouched
            REPAIRED,          // records salvaged and finish() applied
            EMPTY,             // valid prologue but no records to salvage
            INVALID,           // not a file this writer produced
        };
        Status   status = INVALID;
        int      records = 0;
        double   distanceM = 0;
        uint32_t elapsedS = 0;
        time_t   startUtc = 0;
    };

    // Finalizes, in place, a ride left unfinished by a reset or power loss.
    // Replays the record stream to recover the end time and distance, then
    // writes the same tail finish() would have. Safe to call on any file: an
    // already-finished ride is reported as ALREADY_FINISHED and not rewritten.
    static RepairResult repair(fs::FS& fs, const char* path);

private:
    void writeBytes(const uint8_t* data, size_t len);
    void writeDefinition(uint8_t localType, uint16_t globalNum,
                         const uint8_t (*fields)[3], uint8_t fieldCount);

    File file_;
    time_t startUtc_ = 0;
    uint32_t dataSize_ = 0;  // bytes written after the 12-byte header
};
