// Mirrors the id<->path logic in src/map_store.cpp. The property that matters:
// an id saved to disk must come back out of the scan as the SAME id, for every
// layout a card in the field might be in.
#include <cstdio>
#include <cstring>
#include <string>
constexpr int TILE_PREFIX_LEN = 6;
const char TILE_DIR[] = "/maps/tiles";

void tileDirFor(const char* id, char* out, size_t len) {
    int n = 0;
    while (n < TILE_PREFIX_LEN && id[n] && id[n] != '.') ++n;
    if (n < TILE_PREFIX_LEN) { snprintf(out, len, "%s", TILE_DIR); return; }
    snprintf(out, len, "%s/%.*s", TILE_DIR, TILE_PREFIX_LEN, id);
}
// save
std::string savePath(const char* id) {
    char bare[48]; snprintf(bare, sizeof(bare), "%.40s", id);
    if (char* d = strstr(bare, ".ebm")) *d = 0;
    char dir[80]; tileDirFor(bare, dir, sizeof(dir));
    const char* leaf = (strlen(dir) > strlen(TILE_DIR)) ? bare + TILE_PREFIX_LEN : bare;
    char p[96]; snprintf(p, sizeof(p), "%s/%s.ebm", dir, leaf);
    return p;
}
// scan: given subdir name + on-disk filename, rebuild the full id
std::string scanId(const char* subdir, const char* base) {
    char name[20];
    if (subdir[0] && strlen(subdir) == TILE_PREFIX_LEN)
        snprintf(name, sizeof(name), "%s%s", subdir, base);
    else snprintf(name, sizeof(name), "%s", base);
    return name;
}
// read: index entry -> path
std::string readPath(const char* name, const char* dir) {
    char out[96];
    if (dir[0] == 0) { snprintf(out, sizeof(out), "%s/%s", TILE_DIR, name); return out; }
    const char* file = name;
    if (strlen(dir) == TILE_PREFIX_LEN) file += TILE_PREFIX_LEN;
    snprintf(out, sizeof(out), "%s/%s/%s", TILE_DIR, dir, file);
    return out;
}
int fails = 0;
void ck(bool ok, const std::string& what) {
    if (!ok) { printf("  FAIL %s\n", what.c_str()); ++fails; }
}
int main() {
    const char* ids[] = {"862830827ffffff","862830857ffffff","8629a1d97ffffff",
                         "861fb4667ffffff"};
    for (const char* id : ids) {
        std::string p = savePath(id);
        // split the saved path back into subdir + basename, as the scan sees it
        size_t last = p.rfind('/');
        std::string base = p.substr(last + 1);
        std::string rest = p.substr(0, last);
        std::string sub  = (rest == TILE_DIR) ? "" : rest.substr(rest.rfind('/') + 1);
        std::string got  = scanId(sub.c_str(), base.c_str());
        std::string want = std::string(id) + ".ebm";
        ck(got == want, std::string(id) + " scan -> " + got + " (want " + want + ")");
        ck(readPath(got.c_str(), sub.c_str()) == p,
           std::string(id) + " read path -> " + readPath(got.c_str(), sub.c_str()));
        printf("  %s -> %s -> id %s\n", id, p.c_str(), got.c_str());
    }
    // legacy layouts must still resolve
    ck(readPath("862830827ffffff.ebm", "") == "/maps/tiles/862830827ffffff.ebm", "flat card");
    ck(readPath("862830827ffffff.ebm", "a3") == "/maps/tiles/a3/862830827ffffff.ebm", "hash-sharded card");
    printf(fails ? "\n%d FAILED\n" : "\nall round-trips ok\n", fails);
    return fails ? 1 : 0;
}
