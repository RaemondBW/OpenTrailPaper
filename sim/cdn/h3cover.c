// The apps' own H3 cover (companion-ios/Sources/H3/h3shim.c, which Android
// compiles too): the res-6 cells a box selection gives, with each cell's bbox.
// Used by fetch_fixture.py to pick and describe the fixture's hexes.
//
//   h3cover <s> <w> <n> <e>   ->   "<id> <s> <w> <n> <e>" per cell (%.17g)
#include <stdio.h>
#include <stdlib.h>

#include "h3shim.h"

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: h3cover s w n e\n");
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
