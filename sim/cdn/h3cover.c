// The apps' own H3 cover (companion-ios/Sources/H3/h3shim.c, which Android
// compiles too): the res-6 cells a box selection gives, with each cell's bbox.
// Used by fetch_fixture.py to pick and describe the fixture's hexes, and by
// build_poi_fixture.py to put each POI in the one cell containing it.
//
//   h3cover <s> <w> <n> <e>          ->   "<id> <s> <w> <n> <e>" per cell (%.17g)
//   h3cover at <lat> <lon> [...]     ->   "<id>" per point (the cell containing it)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "h3shim.h"

int main(int argc, char **argv) {
    if (argc >= 4 && argc % 2 == 0 && strcmp(argv[1], "at") == 0) {
        for (int i = 2; i + 1 < argc; i += 2) {
            char id[32];
            h3_cell_id(h3_cell_at(atof(argv[i]), atof(argv[i + 1])), id, sizeof id);
            printf("%s\n", id);
        }
        return 0;
    }
    if (argc != 5) {
        fprintf(stderr, "usage: h3cover s w n e | h3cover at lat lon [lat lon ...]\n");
        return 2;
    }
    uint64_t cells[4096];
    int n = h3_covering_cells(atof(argv[1]), atof(argv[2]), atof(argv[3]), atof(argv[4]), cells, 4096);
    for (int i = 0; i < n; ++i) {
        char id[32];
        double s, w, no, e;
        h3_cell_id(cells[i], id, sizeof id);
        h3_cell_bbox(cells[i], &s, &w, &no, &e);
        printf("%s %.17g %.17g %.17g %.17g\n", id, s, w, no, e);
    }
    return 0;
}
