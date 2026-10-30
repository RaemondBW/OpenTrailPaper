#pragma once

#include <stdint.h>

// What the phone last told us it is playing. HOST-SAFE: this header is shared
// by ui_render.cpp (compiled on the host for tools/preview) and the firmware's
// media store (media.h), so nothing here may touch Arduino or FreeRTOS.
//
// `art` is 4-bit panel tones, one byte per pixel (0x00/0x11/0x22/0x33/0xFF —
// the only values this glass renders distinctly, see the dither in media.cpp),
// row-major artW x artH. The phone sends 8-bit grayscale; the device dithers
// once on receipt so the renderer just blits.
struct MediaState {
    bool present = false;       // a player exists on the phone
    bool playing = false;
    uint16_t posSec = 0;        // position when the phone last reported
    uint16_t durSec = 0;        // 0 = unknown / live
    uint32_t posAtMs = 0;       // millis() of that report, to advance locally
    // The same report to the millisecond (AMS sends "143.5"), and the rate it
    // advances at while playing: 1.0 normally, 2.0 fast-forwarding, negative
    // rewinding. posSec stays posMs / 1000 for the renderer.
    uint32_t posMs = 0;
    float rate = 1.0f;
    char title[64] = "";
    char artist[64] = "";
    char album[64] = "";
    const uint8_t* art = nullptr;
    int artW = 0, artH = 0;

    // Where playback is at `now` (millis()): the last report, advanced at
    // `rate` while playing — AMS reports on state changes and seeks only, not
    // as the track plays. Clamped to [0, duration] when the duration is known.
    // The device's progress bar and the live stream's MUSIC record both read
    // this, so the phone and the glass agree.
    uint32_t positionMs(uint32_t now) const {
        double p = posMs;
        if (playing) p += (double)(uint32_t)(now - posAtMs) * rate;
        if (p < 0) p = 0;
        const double dur = (double)durSec * 1000.0;
        if (durSec > 0 && p > dur) p = dur;
        return (uint32_t)p;
    }
};

// Control commands the device sends back to the phone (BLE notify payload).
enum MediaCmd : uint8_t {
    MC_TOGGLE = 1,   // play/pause
    MC_NEXT = 2,
    MC_PREV = 3,
    MC_VOL_UP = 4,   // system volume — works whatever app is playing
    MC_VOL_DOWN = 5,
};
