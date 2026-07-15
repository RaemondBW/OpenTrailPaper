// Exposes the vendored H3 C library (Sources/H3) to Swift. H3 res-6 cells are
// the map tiling unit: the app covers a drawn area with hexagons, sends each
// missing one to the device, and the device stores them by H3 id.
#import "h3api.h"
#import "h3shim.h"
