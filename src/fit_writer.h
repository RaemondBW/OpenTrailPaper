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
    // One data point, 1 Hz. Use the INVALID_* constants for missing values.
    struct Record {
        time_t   utc;
        double   latitudeDeg;
        double   longitudeDeg;
        float    altitudeM;
        float    speedMs;
        double   distanceM;
        uint16_t powerW;      // INVALID_U16 if absent
        uint8_t  heartRate;   // INVALID_U8 if absent
        uint8_t  cadence;     // INVALID_U8 if absent
    };

    static constexpr uint16_t INVALID_U16 = 0xFFFF;
    static constexpr uint8_t  INVALID_U8 = 0xFF;

    // Opens the file and writes header, file_id and timer-start event.
    bool begin(fs::FS& fs, const char* path, time_t startUtc);

    void writeRecord(const Record& r);

    // Call periodically so a crash mid-ride loses seconds, not the ride.
    // Also patches the header's data_size to the bytes written so far, so a
    // ride cut short by power loss is still recoverable without finish().
    void checkpoint();

    // Timer-stop event, lap, session, activity, then header/CRC fixup.
    bool finish(time_t endUtc, double totalDistanceM, uint32_t timerS);

    bool isOpen() const { return (bool)file_; }

private:
    void writeBytes(const uint8_t* data, size_t len);
    void writeDefinition(uint8_t localType, uint16_t globalNum,
                         const uint8_t (*fields)[3], uint8_t fieldCount);

    File file_;
    time_t startUtc_ = 0;
    uint32_t dataSize_ = 0;  // bytes written after the 12-byte header
};
