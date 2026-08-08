#include "dash_layout.h"

#include <string.h>
#include <stdio.h>

namespace {

// Field id (config file + BLE) and the caption drawn above the value. Indexed
// by DashField, so the order here must match the enum.
struct FieldInfo { const char* id; const char* label; };
const FieldInfo kFields[DF_COUNT] = {
    {"speed",      "SPEED"},
    {"power3s",    "POWER · 3S"},
    {"power",      "POWER"},
    {"hr",         "HEART RATE"},
    {"cadence",    "CADENCE"},
    {"distance",   "DISTANCE"},
    {"ridetime",   "RIDE TIME"},
    {"movingtime", "MOVING TIME"},
    {"climb",      "CLIMB"},
    {"grade",      "GRADE"},
    {"altitude",   "ALTITUDE"},
    {"battery",    "BATTERY"},
    {"sats",       "SATELLITES"},
    {"clock",      "CLOCK"},
    {"routeleft",  "ROUTE LEFT"},
};

const char* kSizes[DZ_COUNT] = {"small", "medium", "large", "hero"};

bool tokenEq(const char* tok, size_t len, const char* candidate) {
    return strlen(candidate) == len && strncmp(tok, candidate, len) == 0;
}

}  // namespace

const DashLayout& dashDefaultLayout() {
    // Field for field what ui_render_dashboard drew before it was configurable:
    // the 3 s power hero, then heart rate / cadence and ride time / distance as
    // two half-width rows. A device with no config file must not look different.
    static DashLayout d = [] {
        DashLayout l;
        l.items[0] = {DF_POWER3S,    DZ_HERO,   false};
        l.items[1] = {DF_HEART_RATE, DZ_MEDIUM, true};
        l.items[2] = {DF_CADENCE,    DZ_MEDIUM, true};
        l.items[3] = {DF_RIDE_TIME,  DZ_MEDIUM, true};
        l.items[4] = {DF_DISTANCE,   DZ_MEDIUM, true};
        l.count = 5;
        return l;
    }();
    return d;
}

const char* dashFieldId(uint8_t field) {
    return field < DF_COUNT ? kFields[field].id : "";
}

const char* dashFieldLabel(uint8_t field) {
    return field < DF_COUNT ? kFields[field].label : "";
}

const char* dashSizeId(uint8_t size) {
    return size < DZ_COUNT ? kSizes[size] : "";
}

uint8_t dashFieldFromId(const char* id, size_t len) {
    for (uint8_t i = 0; i < DF_COUNT; ++i)
        if (tokenEq(id, len, kFields[i].id)) return i;
    return DF_COUNT;
}

uint8_t dashSizeFromId(const char* id, size_t len) {
    for (uint8_t i = 0; i < DZ_COUNT; ++i)
        if (tokenEq(id, len, kSizes[i])) return i;
    return DZ_COUNT;
}

bool dashParse(const char* text, DashLayout& out) {
    out.count = 0;
    if (!text) return false;

    const char* p = text;
    while (*p && out.count < DASH_MAX_ITEMS) {
        // One line at a time; `#` comments out the rest of it.
        const char* lineEnd = p;
        while (*lineEnd && *lineEnd != '\n') ++lineEnd;
        const char* cut = p;
        while (cut < lineEnd && *cut != '#') ++cut;

        // Up to three whitespace-separated tokens: field, size, "half".
        const char* tok[3] = {nullptr, nullptr, nullptr};
        size_t tokLen[3] = {0, 0, 0};
        int ntok = 0;
        const char* q = p;
        while (q < cut && ntok < 3) {
            while (q < cut && (*q == ' ' || *q == '\t' || *q == '\r')) ++q;
            if (q >= cut) break;
            const char* start = q;
            while (q < cut && *q != ' ' && *q != '\t' && *q != '\r') ++q;
            tok[ntok] = start;
            tokLen[ntok] = (size_t)(q - start);
            ++ntok;
        }

        if (ntok >= 1) {
            uint8_t f = dashFieldFromId(tok[0], tokLen[0]);
            // Skip the line, keep the file. A hand-edited config with one
            // misspelled field should cost that row, not the whole layout —
            // otherwise a typo silently reverts the rider to the default and
            // there is nothing on the panel to say why.
            if (f < DF_COUNT) {
                DashItem it;
                it.field = f;
                it.size = DZ_MEDIUM;
                it.half = false;
                if (ntok >= 2) {
                    uint8_t z = dashSizeFromId(tok[1], tokLen[1]);
                    if (z < DZ_COUNT) it.size = z;
                }
                for (int i = 1; i < ntok; ++i)
                    if (tokenEq(tok[i], tokLen[i], "half")) it.half = true;
                out.items[out.count++] = it;
            }
        }

        if (!*lineEnd) break;
        p = lineEnd + 1;
    }

    // An empty result means the file was blank, all comments, or all typos —
    // in every case the caller is better off with the default than a blank panel.
    return out.count > 0;
}

size_t dashSerialize(const DashLayout& layout, char* out, size_t cap) {
    if (!out || cap == 0) return 0;
    static const char kHeader[] =
        "# OpenTrailPaper dashboard layout\n"
        "# <field> <small|medium|large|hero> [half]\n"
        "# 'half' shares the row with the next 'half' field.\n";
    size_t n = 0;
    int w = snprintf(out, cap, "%s", kHeader);
    if (w < 0 || (size_t)w >= cap) return 0;
    n = (size_t)w;
    for (int i = 0; i < layout.count; ++i) {
        const DashItem& it = layout.items[i];
        if (it.field >= DF_COUNT || it.size >= DZ_COUNT) continue;
        w = snprintf(out + n, cap - n, "%-10s %-6s%s\n", dashFieldId(it.field),
                     dashSizeId(it.size), it.half ? " half" : "");
        if (w < 0 || (size_t)w >= cap - n) return 0;   // truncated: report failure
        n += (size_t)w;
    }
    return n;
}
