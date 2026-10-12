// h3tool — the apps' own H3 math for the server-side tile builder.
//
// The prebuilt tiles must carry the exact bytes the phone apps write, and two
// of those bytes-sources are H3 geometry: the cell bounding box (it feeds the
// .ebm grid origin, the ELV1 header and the .poi origin) and "which cell is
// this POI in". The apps get both from the vendored H3 C through
// companion-ios/Sources/H3/h3shim.c (Android compiles the same shim). h3-js is
// the same library compiled to JS, but h3shim converts radians with
// `r * 180.0 / M_PI` where H3's own helpers use `r * M_180_PI`, which differs
// in the last bit often enough to matter for a byte comparison. So the builder
// asks this program, built from the very same C sources, instead.
//
// Build (tools/tiles/build_h3tool.sh):
//   cc -O2 -std=c11 -I <H3> -I <H3>/include h3tool.c <H3>/h3shim.c <H3>/lib/*.c -lm
//
// Usage (batch, stdin -> stdout, one record per line):
//   h3tool bbox   "<h3id>"            -> "<h3id> <s> <w> <n> <e>"   (%.17g)
//   h3tool cell   "<lat> <lon>"       -> "<h3id>" (res 6, as h3_cell_at)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h3shim.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: h3tool bbox|cell < input\n");
        return 2;
    }
    static char line[256];
    char id[32];
    if (strcmp(argv[1], "bbox") == 0) {
        while (fgets(line, sizeof line, stdin)) {
            if (sscanf(line, "%31s", id) != 1) continue;
            uint64_t c = h3_from_id(id);
            if (!c) { printf("%s ERR\n", id); continue; }
            double s, w, n, e;
            h3_cell_bbox(c, &s, &w, &n, &e);
            printf("%s %.17g %.17g %.17g %.17g\n", id, s, w, n, e);
        }
    } else if (strcmp(argv[1], "cell") == 0) {
        while (fgets(line, sizeof line, stdin)) {
            double la, lo;
            if (sscanf(line, "%lf %lf", &la, &lo) != 2) { printf("0\n"); continue; }
            uint64_t c = h3_cell_at(la, lo);
            if (!c) { printf("0\n"); continue; }
            h3_cell_id(c, id, sizeof id);
            printf("%s\n", id);
        }
    } else {
        fprintf(stderr, "unknown mode %s\n", argv[1]);
        return 2;
    }
    return 0;
}
