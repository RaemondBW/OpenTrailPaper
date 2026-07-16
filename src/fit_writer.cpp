#include "fit_writer.h"

namespace {

constexpr uint32_t FIT_EPOCH_OFFSET = 631065600;  // 1989-12-31T00:00:00Z
constexpr int32_t  INVALID_S32 = 0x7FFFFFFF;
constexpr double   SEMICIRCLES_PER_DEG = 2147483648.0 / 180.0;

// Base types
constexpr uint8_t T_ENUM = 0x00;
constexpr uint8_t T_U8   = 0x02;
constexpr uint8_t T_S32  = 0x85;
constexpr uint8_t T_U16  = 0x84;
constexpr uint8_t T_U32  = 0x86;
constexpr uint8_t T_U32Z = 0x8C;

// Global message numbers
constexpr uint16_t MSG_FILE_ID  = 0;
constexpr uint16_t MSG_SESSION  = 18;
constexpr uint16_t MSG_LAP      = 19;
constexpr uint16_t MSG_RECORD   = 20;
constexpr uint16_t MSG_EVENT    = 21;
constexpr uint16_t MSG_ACTIVITY = 34;

// Local message types (assigned once, definitions written up front)
constexpr uint8_t L_FILE_ID  = 0;
constexpr uint8_t L_RECORD   = 1;
constexpr uint8_t L_EVENT    = 2;
constexpr uint8_t L_LAP      = 3;
constexpr uint8_t L_SESSION  = 4;
constexpr uint8_t L_ACTIVITY = 5;

// {field number, size, base type}
const uint8_t FIELDS_FILE_ID[][3] = {
    {0, 1, T_ENUM},   // type = 4 (activity)
    {1, 2, T_U16},    // manufacturer = 255 (development)
    {2, 2, T_U16},    // product
    {3, 4, T_U32Z},   // serial_number
    {4, 4, T_U32},    // time_created
};

const uint8_t FIELDS_RECORD[][3] = {
    {253, 4, T_U32},  // timestamp
    {0, 4, T_S32},    // position_lat (semicircles)
    {1, 4, T_S32},    // position_long
    {2, 2, T_U16},    // altitude ((m + 500) * 5)
    {5, 4, T_U32},    // distance (cm)
    {6, 2, T_U16},    // speed (mm/s)
    {7, 2, T_U16},    // power (W)
    {3, 1, T_U8},     // heart_rate (bpm)
    {4, 1, T_U8},     // cadence (rpm)
};

const uint8_t FIELDS_EVENT[][3] = {
    {253, 4, T_U32},  // timestamp
    {0, 1, T_ENUM},   // event = 0 (timer)
    {1, 1, T_ENUM},   // event_type
    {4, 1, T_U8},     // event_group
};

const uint8_t FIELDS_LAP[][3] = {
    {253, 4, T_U32},  // timestamp
    {2, 4, T_U32},    // start_time
    {7, 4, T_U32},    // total_elapsed_time (ms)
    {8, 4, T_U32},    // total_timer_time (ms)
    {9, 4, T_U32},    // total_distance (cm)
    {254, 2, T_U16},  // message_index
    {0, 1, T_ENUM},   // event = 9 (lap)
    {1, 1, T_ENUM},   // event_type = 1 (stop)
};

const uint8_t FIELDS_SESSION[][3] = {
    {253, 4, T_U32},  // timestamp
    {2, 4, T_U32},    // start_time
    {7, 4, T_U32},    // total_elapsed_time (ms)
    {8, 4, T_U32},    // total_timer_time (ms)
    {9, 4, T_U32},    // total_distance (cm)
    {25, 2, T_U16},   // first_lap_index
    {26, 2, T_U16},   // num_laps
    {0, 1, T_ENUM},   // event = 8 (session)
    {1, 1, T_ENUM},   // event_type = 1 (stop)
    {5, 1, T_ENUM},   // sport = 2 (cycling)
    {6, 1, T_ENUM},   // sub_sport = 0
};

const uint8_t FIELDS_ACTIVITY[][3] = {
    {253, 4, T_U32},  // timestamp
    {0, 4, T_U32},    // total_timer_time (ms)
    {5, 4, T_U32},    // local_timestamp
    {1, 2, T_U16},    // num_sessions
    {2, 1, T_ENUM},   // type = 0 (manual)
    {3, 1, T_ENUM},   // event = 26 (activity)
    {4, 1, T_ENUM},   // event_type = 1 (stop)
};

uint16_t crc16(uint16_t crc, uint8_t byte) {
    static const uint16_t table[16] = {
        0x0000, 0xCC01, 0xD801, 0x1400, 0xF001, 0x3C00, 0x2800, 0xE401,
        0xA001, 0x6C00, 0x7800, 0xB401, 0x5000, 0x9C01, 0x8801, 0x4400,
    };
    uint16_t tmp = table[crc & 0xF];
    crc = (crc >> 4) & 0x0FFF;
    crc = crc ^ tmp ^ table[byte & 0xF];
    tmp = table[crc & 0xF];
    crc = (crc >> 4) & 0x0FFF;
    crc = crc ^ tmp ^ table[(byte >> 4) & 0xF];
    return crc;
}

uint32_t fitTime(time_t utc) { return (uint32_t)(utc - FIT_EPOCH_OFFSET); }
time_t   utcFromFit(uint32_t fit) { return (time_t)fit + FIT_EPOCH_OFFSET; }

void put16(uint8_t* p, uint16_t v) { p[0] = v & 0xFF; p[1] = v >> 8; }
void put32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}
uint32_t get32(const uint8_t* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

// begin() always emits the same prologue, so its length is fixed: the 12-byte
// header, then the file_id definition + message, the timer-start event
// definition + message, and the record definition. Records follow at a fixed
// stride. repair() relies on both to find where the record stream starts and
// how far it got. Keep in sync with begin() / writeRecord().
constexpr uint32_t PROLOGUE_BYTES = 12 +          // header
                                    (6 + 5 * 3) + // file_id definition
                                    14 +          // file_id message
                                    (6 + 4 * 3) + // event definition
                                    8 +           // timer-start event
                                    (6 + 9 * 3);  // record definition
constexpr uint32_t RECORD_BYTES = 25;

// Byte offsets within a record message, mirroring writeRecord().
constexpr uint32_t REC_OFF_TIMESTAMP = 1;
constexpr uint32_t REC_OFF_DISTANCE  = 15;

}  // namespace

void FitWriter::writeBytes(const uint8_t* data, size_t len) {
    file_.write(data, len);
    dataSize_ += len;
}

void FitWriter::writeDefinition(uint8_t localType, uint16_t globalNum,
                                const uint8_t (*fields)[3], uint8_t fieldCount) {
    uint8_t buf[6 + 32 * 3];
    buf[0] = 0x40 | localType;  // definition message header
    buf[1] = 0;                 // reserved
    buf[2] = 0;                 // little endian
    put16(&buf[3], globalNum);
    buf[5] = fieldCount;
    for (uint8_t i = 0; i < fieldCount; ++i) {
        memcpy(&buf[6 + i * 3], fields[i], 3);
    }
    writeBytes(buf, 6 + fieldCount * 3);
}

bool FitWriter::begin(fs::FS& fs, const char* path, time_t startUtc) {
    // "w+", not FILE_WRITE ("w"): finish() computes the trailing CRC by
    // re-reading the file, and a write-only handle reads back zero bytes.
    file_ = fs.open(path, "w+");
    if (!file_) return false;
    startUtc_ = startUtc;
    dataSize_ = 0;

    // Legacy 12-byte header; data_size patched in finish().
    uint8_t hdr[12] = {12, 0x10, 0, 0, 0, 0, 0, 0, '.', 'F', 'I', 'T'};
    put16(&hdr[2], 2132);  // profile version
    file_.write(hdr, sizeof(hdr));

    writeDefinition(L_FILE_ID, MSG_FILE_ID, FIELDS_FILE_ID, 5);
    uint8_t fid[14];
    fid[0] = L_FILE_ID;
    fid[1] = 4;                       // type: activity
    put16(&fid[2], 255);              // manufacturer: development
    put16(&fid[4], 1);                // product
    put32(&fid[6], 0x54355333);       // serial ("T5S3")
    put32(&fid[10], fitTime(startUtc));
    writeBytes(fid, sizeof(fid));

    writeDefinition(L_EVENT, MSG_EVENT, FIELDS_EVENT, 4);
    uint8_t evt[8];
    evt[0] = L_EVENT;
    put32(&evt[1], fitTime(startUtc));
    evt[5] = 0;  // event: timer
    evt[6] = 0;  // event_type: start
    evt[7] = 0;
    writeBytes(evt, sizeof(evt));

    writeDefinition(L_RECORD, MSG_RECORD, FIELDS_RECORD, 9);
    return true;
}

void FitWriter::checkpoint() {
    if (!file_) return;
    uint32_t pos = file_.position();     // current end of data
    file_.seek(4);                       // header data_size field
    uint8_t sz[4];
    put32(sz, dataSize_);
    file_.write(sz, 4);
    file_.seek(pos);                     // resume appending records
    file_.flush();
}

void FitWriter::writeRecord(const Record& r) {
    if (!file_) return;
    uint8_t buf[25];
    buf[0] = L_RECORD;
    put32(&buf[1], fitTime(r.utc));
    put32(&buf[5], (uint32_t)(int32_t)llround(r.latitudeDeg * SEMICIRCLES_PER_DEG));
    put32(&buf[9], (uint32_t)(int32_t)llround(r.longitudeDeg * SEMICIRCLES_PER_DEG));
    put16(&buf[13], (uint16_t)lround((r.altitudeM + 500.0f) * 5.0f));
    put32(&buf[15], (uint32_t)llround(r.distanceM * 100.0));
    put16(&buf[19], (uint16_t)lround(r.speedMs * 1000.0f));
    put16(&buf[21], r.powerW);
    buf[23] = r.heartRate;
    buf[24] = r.cadence;
    writeBytes(buf, sizeof(buf));
}

FitWriter::RepairResult FitWriter::repair(fs::FS& fs, const char* path) {
    RepairResult res;

    File f = fs.open(path, "r+");  // read/write, no truncate
    if (!f) return res;            // INVALID
    uint32_t fileSize = f.size();
    if (fileSize < PROLOGUE_BYTES) { f.close(); return res; }

    uint8_t prologue[PROLOGUE_BYTES];
    if (f.read(prologue, PROLOGUE_BYTES) != (int)PROLOGUE_BYTES) {
        f.close();
        return res;
    }
    if (prologue[0] != 12 || memcmp(&prologue[8], ".FIT", 4) != 0) {
        f.close();
        return res;  // not one of ours
    }

    // A finished ride is exactly header + data_size + the 2-byte CRC. Anything
    // else is a ride that was still being written when the device went down;
    // its data_size only reflects the last checkpoint, so don't trust it.
    uint32_t headerDataSize = get32(&prologue[4]);
    if (fileSize == 12 + headerDataSize + 2) {
        f.close();
        res.status = RepairResult::ALREADY_FINISHED;
        return res;
    }

    res.startUtc = utcFromFit(get32(&prologue[43]));  // file_id.time_created

    // Walk the record stream. A reset can leave a torn final record, so only
    // whole ones count; the tail written below overwrites any partial bytes.
    uint32_t records = (fileSize - PROLOGUE_BYTES) / RECORD_BYTES;
    time_t   endUtc = res.startUtc;
    double   distanceM = 0;
    uint32_t good = 0;
    uint8_t  rec[RECORD_BYTES];
    for (uint32_t i = 0; i < records; ++i) {
        f.seek(PROLOGUE_BYTES + i * RECORD_BYTES);
        if (f.read(rec, RECORD_BYTES) != (int)RECORD_BYTES) break;
        if (rec[0] != L_RECORD) break;  // stream desynced — salvage what we have
        endUtc = utcFromFit(get32(&rec[REC_OFF_TIMESTAMP]));
        distanceM = get32(&rec[REC_OFF_DISTANCE]) / 100.0;
        good = i + 1;
    }

    if (good == 0) {
        f.close();
        res.status = RepairResult::EMPTY;
        return res;
    }

    res.records = (int)good;
    res.distanceM = distanceM;
    // The true timer count died with the reset; elapsed time is the honest
    // stand-in, and matches what the record stream actually spans.
    res.elapsedS = (uint32_t)(endUtc - res.startUtc);

    // Hand the open handle to a writer positioned just past the last whole
    // record, so finish() appends the usual tail and fixes up size + CRC.
    FitWriter w;
    w.file_ = f;
    w.startUtc_ = res.startUtc;
    w.dataSize_ = PROLOGUE_BYTES - 12 + good * RECORD_BYTES;
    if (!w.file_.seek(PROLOGUE_BYTES + good * RECORD_BYTES)) {
        f.close();
        return res;  // INVALID
    }
    if (!w.finish(endUtc, distanceM, res.elapsedS)) return res;

    res.status = RepairResult::REPAIRED;
    return res;
}

bool FitWriter::finish(time_t endUtc, double totalDistanceM, uint32_t timerS) {
    if (!file_) return false;

    uint32_t ts = fitTime(endUtc);
    uint32_t start = fitTime(startUtc_);
    uint32_t elapsedMs = (ts - start) * 1000;
    uint32_t timerMs = timerS * 1000;
    uint32_t distCm = (uint32_t)llround(totalDistanceM * 100.0);

    uint8_t evt[8];
    evt[0] = L_EVENT;
    put32(&evt[1], ts);
    evt[5] = 0;  // timer
    evt[6] = 4;  // stop_all
    evt[7] = 0;
    writeBytes(evt, sizeof(evt));

    writeDefinition(L_LAP, MSG_LAP, FIELDS_LAP, 8);
    uint8_t lap[25];
    lap[0] = L_LAP;
    put32(&lap[1], ts);
    put32(&lap[5], start);
    put32(&lap[9], elapsedMs);
    put32(&lap[13], timerMs);
    put32(&lap[17], distCm);
    put16(&lap[21], 0);  // message_index
    lap[23] = 9;         // event: lap
    lap[24] = 1;         // event_type: stop
    writeBytes(lap, sizeof(lap));

    writeDefinition(L_SESSION, MSG_SESSION, FIELDS_SESSION, 11);
    uint8_t ses[29];
    ses[0] = L_SESSION;
    put32(&ses[1], ts);
    put32(&ses[5], start);
    put32(&ses[9], elapsedMs);
    put32(&ses[13], timerMs);
    put32(&ses[17], distCm);
    put16(&ses[21], 0);  // first_lap_index
    put16(&ses[23], 1);  // num_laps
    ses[25] = 8;         // event: session
    ses[26] = 1;         // event_type: stop
    ses[27] = 2;         // sport: cycling
    ses[28] = 0;         // sub_sport: generic
    writeBytes(ses, sizeof(ses));

    writeDefinition(L_ACTIVITY, MSG_ACTIVITY, FIELDS_ACTIVITY, 7);
    uint8_t act[16];
    act[0] = L_ACTIVITY;
    put32(&act[1], ts);
    put32(&act[5], timerMs);
    put32(&act[9], ts);  // local_timestamp (UTC; timezone handling later)
    put16(&act[13], 1);  // num_sessions
    // type=0 manual is what most head units emit here
    // (activity.type distinguishes manual vs auto multisport)
    act[15] = 0;
    writeBytes(act, sizeof(act));
    uint8_t tail[2] = {26, 1};  // event: activity, event_type: stop
    writeBytes(tail, sizeof(tail));

    // Patch data_size, then CRC the whole file (header + data) by re-reading.
    file_.flush();
    file_.seek(4);
    uint8_t sz[4];
    put32(sz, dataSize_);
    file_.write(sz, 4);
    file_.flush();

    file_.seek(0);
    uint16_t crc = 0;
    uint8_t chunk[256];
    size_t n;
    while ((n = file_.read(chunk, sizeof(chunk))) > 0) {
        for (size_t i = 0; i < n; ++i) crc = crc16(crc, chunk[i]);
    }
    file_.seek(12 + dataSize_);
    uint8_t crcBuf[2];
    put16(crcBuf, crc);
    file_.write(crcBuf, 2);
    file_.close();
    return true;
}
