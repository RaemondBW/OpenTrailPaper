// Runs the FIRMWARE's own album-art code (src/media.cpp, compiled for the
// host) and prints the tone index of every pixel, so the Swift port can be
// compared byte for byte. Usage: firmware_dither <w> <h> <recipe>
//        firmware_dither unpack <w> <h>   (packed tone art on stdin)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include "media.h"

static uint8_t px(int recipe, int x, int y, int w, int h, uint32_t& rng) {
    switch (recipe) {
    case 0: return (uint8_t)((x * 255) / (w > 1 ? w - 1 : 1));            // ramp
    case 1: return (uint8_t)(((x + y) * 255) / (w + h - 2 > 0 ? w + h - 2 : 1));
    case 2: rng = rng * 1103515245u + 12345u; return (uint8_t)(rng >> 16); // noise
    case 3: return 200;                                                    // flat
    default: return (uint8_t)((x * 31 + y * 17) & 0xFF);                   // stripes
    }
}

static int toneIndex(uint8_t t) {
    switch (t) { case 0x00: return 0; case 0x11: return 1; case 0x22: return 2;
                 case 0x33: return 3; default: return 4; }
}

int main(int argc, char** argv) {
    if (argc == 4 && strcmp(argv[1], "unpack") == 0) {
        int w = atoi(argv[2]), h = atoi(argv[3]);
        std::vector<uint8_t> in((w * h + 2) / 3);
        if (fread(in.data(), 1, in.size(), stdin) != in.size()) return 2;
        media::beginToneArt(w, h);
        media::artData(in.data(), in.size());
        media::commitArt();
    } else if (argc == 4) {
        int w = atoi(argv[1]), h = atoi(argv[2]), recipe = atoi(argv[3]);
        std::vector<uint8_t> g(w * h);
        uint32_t rng = 1;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) g[y * w + x] = px(recipe, x, y, w, h, rng);
        media::beginArt(w, h);
        media::artData(g.data(), g.size());
        media::commitArt();
    } else {
        return 2;
    }
    const MediaState& s = media::get();
    if (!s.art) { fprintf(stderr, "no art published\n"); return 1; }
    for (int i = 0; i < s.artW * s.artH; ++i) putchar('0' + toneIndex(s.art[i]));
    return 0;
}
