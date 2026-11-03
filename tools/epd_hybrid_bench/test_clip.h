// A synthetic clip that exists to break the refresh, one way per section. Rendered
// into PSRAM once and played back from there, so playback cost is constant and
// every mode gets exactly the same frames.
//
// Half resolution, pixel-doubled to the panel: 16 KB a frame. That also means no
// section exercises single-pixel dither, which is what the `pan` scene is for.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace test_clip {

struct Section {
    const char* name;
    const char* tests;
    int first;
    int frames;
};

constexpr int kSections = 6;
extern const Section kSection[kSections];
int frameCount();

// Allocates and renders the clip. Safe to call again; it only renders once.
bool render();

// Writes clip frame `i` into a packed 2bpp panel frame.
void expand(int i, uint8_t* packed);

// A white half and a 50% half, held between sections. Anything that shows on it
// is left over from the section before.
void expandCard(uint8_t* packed);

}  // namespace test_clip
