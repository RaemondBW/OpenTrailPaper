// Checks the invariant the whole index-free tile lookup rests on: a position
// resolves to the H3 cell id that NAMES the file on the card, and that id turns
// back into the path the file actually sits at.
//
// If this is wrong the device renders nothing — it would be looking for files
// that no encoder ever wrote — and nothing else in the system would complain.
// Run against a real tile directory (the layout the website generator and the
// phone both produce): tools/map_test/run_h3lookup.sh <tiledir>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <sys/stat.h>

#include "map_select.h"

namespace {

constexpr int TILE_PREFIX_LEN = 6;

// Mirrors tilePathForId() in map_store.cpp.
std::string tilePathForId(const std::string& id) {
    if (id.size() > TILE_PREFIX_LEN)
        return id.substr(0, TILE_PREFIX_LEN) + "/" + id.substr(TILE_PREFIX_LEN) + ".ebm";
    return id + ".ebm";
}

int fails = 0;
void ck(bool ok, const std::string& what) {
    if (!ok) { printf("  FAIL %s\n", what.c_str()); ++fails; }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) { fprintf(stderr, "usage: h3lookup <tiledir>\n"); return 1; }
    const std::string root = argv[1];

    // Every tile actually on the card, by id.
    std::vector<std::string> ids;
    DIR* d = opendir(root.c_str());
    if (!d) { fprintf(stderr, "cannot open %s\n", root.c_str()); return 1; }
    for (dirent* e = readdir(d); e; e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string sub = root + "/" + e->d_name;
        struct stat st;
        if (stat(sub.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
        DIR* d2 = opendir(sub.c_str());
        if (!d2) continue;
        for (dirent* f = readdir(d2); f; f = readdir(d2)) {
            std::string leaf(f->d_name);
            if (leaf.find(".ebm") == std::string::npos) continue;
            ids.push_back(std::string(e->d_name) + leaf.substr(0, leaf.find(".ebm")));
        }
        closedir(d2);
    }
    closedir(d);
    printf("%zu tiles on the card\n", ids.size());
    if (ids.empty()) return 1;

    for (const std::string& id : ids) {
        uint64_t cell = h3_from_id(id.c_str());
        ck(cell != 0, id + ": id does not parse as an H3 cell");
        if (!cell) continue;

        // The id must round-trip, or the path we build is not the file's name.
        char back[24];
        h3_cell_id(cell, back, sizeof(back));
        ck(id == back, id + ": id round-trip gave " + back);

        // A position inside the cell must select that cell — and first, since
        // the rider is standing in it.
        double s, w, n, e;
        h3_cell_bbox(cell, &s, &w, &n, &e);
        const double lat = (s + n) / 2, lon = (w + e) / 2;
        ck(h3_cell_at(lat, lon) == cell, id + ": cell_at(centre) is a different cell");

        uint64_t sel[MAP_TILE_BUDGET];
        int got = mapSelectCells(lat, lon, 2.0f, 0.0f, MAP_TILE_BUDGET, sel);
        ck(got > 0 && sel[0] == cell,
           id + ": mapSelectCells did not put the rider's own cell first");

        // And the path we would open has to be where the file is.
        std::string want = root + "/" + tilePathForId(id);
        struct stat st;
        ck(stat(want.c_str(), &st) == 0, id + ": no file at " + want);
    }

    // The widest track-up view must still fit the budget, or the map loses its
    // outer ring exactly when the rider asked for the overview.
    uint64_t sel[MAP_TILE_BUDGET];
    int overlapping = 0;
    uint64_t any = h3_from_id(ids[0].c_str());
    double s, w, n, e;
    h3_cell_bbox(any, &s, &w, &n, &e);
    int got = mapSelectCells((s + n) / 2, (w + e) / 2, 32.0f, -45.0f,
                             MAP_TILE_BUDGET, sel, &overlapping);
    printf("widest track-up view: %d cells cover it, %d selected (budget %d)\n",
           overlapping, got, MAP_TILE_BUDGET);
    ck(overlapping <= MAP_TILE_BUDGET,
       "widest view needs more cells than the budget allows");

    printf(fails ? "\n%d FAILED\n" : "\nall lookups ok\n", fails);
    return fails ? 1 : 0;
}
