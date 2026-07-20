#pragma once

// Map screen renderer (design 1f: 1-bit track-up map). Takes features
// already projected to screen coordinates — the v2 tile loader will
// produce MapScreenData from OSM tiles on the SD card; tools/preview
// feeds it a synthetic scene today.
//
// Same host/device split as ui_render.h: pure pixels, no hardware.

#include <cstdint>

struct RideState;

// Road tiers (EBM2). Numbers are the on-tile class bytes.
enum MapFeatureClass : uint8_t {
    MAP_ROAD_MAJOR = 0,      // arterial: motorway/trunk — never shed
    MAP_ROAD_PRIMARY = 1,    // primary — never shed (carries the overview)
    MAP_ROAD_SECONDARY = 2,  // secondary — shed at ≥32 m/px
    MAP_ROAD_TERTIARY = 3,   // tertiary — shed at ≥32 m/px
    MAP_ROAD_MINOR = 4,      // residential/etc — shed at ≥16 m/px
    MAP_PATH = 5,            // trails — light dither, shed at ≥4 m/px
    MAP_WATER = 6,           // water bodies (WTR2 section) — filled dot dither
    MAP_PARK = 7,            // parks/green (PRK2 section) — filled hatch dither
};

struct MapPolyline {
    MapFeatureClass cls;
    const int16_t* pts;  // x0,y0,x1,y1,... screen px (portrait 540x960)
    int pointCount;
};

struct MapScreenData {
    const MapPolyline* features;
    int featureCount;

    // Water bodies (from the tile WTR2 section), projected to screen; drawn as
    // a light dot-dithered fill under the roads.
    const MapPolyline* water = nullptr;
    int waterCount = 0;

    // Parks/green areas (from the tile PRK2 section); drawn as a hatch-dithered
    // fill beneath the water + roads. Distinct dither so it reads apart from water.
    const MapPolyline* parks = nullptr;
    int parkCount = 0;

    // Route polyline; the first riddenPointCount points render solid
    // (already ridden), the rest dashed (ahead) per the design.
    const int16_t* route;
    int routePointCount;
    int riddenPointCount;

    int riderX, riderY;    // rider marker, screen px
    float headingDeg;      // 0 = up/north, clockwise

    float metersPerPixel;  // for the scale bar

    // When a route is loaded the footer's TIME cell becomes LEFT KM.
    bool showRemaining;
    float remainingKm;

    // Screen direction of true north (0 = up; track-up sets -heading).
    float northDeg;
    bool trackUp;

    // Position is coming from the connected phone (device GPS has no fix).
    bool phonePosition = false;

    // A map actually covers the current position. When false the map screen
    // shows a "no map here — download it in the app" prompt.
    bool hasMap = true;

    // How many SD tiles were projected this frame (diagnostics/timing).
    int projectedTiles = 0;
    int tilePolys = 0;          // polys from tiles (before the base blob)
    int clsCount[7] = {0, 0, 0, 0, 0, 0, 0};  // kept polys per class
};

// Compass touch target (tap toggles north-up / track-up)
struct MapCompassZone {
    int cx, cy, r;
};
extern const MapCompassZone kMapCompass;

// Touch targets (zoom buttons on the right edge of the map area)
struct MapTouchZones {
    int zoomX, zoomInY, zoomOutY, size;
};
extern const MapTouchZones kMapZoom;

void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb);

// Just the map (features + route + rider), full-screen, no chrome. Used as the
// backdrop behind the powered-off screen.
void ui_render_map_features(const MapScreenData& map, const RideState& s,
                            uint8_t* fb);

// Whole active route fitted into the area above the accept sheet, for the
// "Start navigation?" preview.
void ui_render_route_preview(uint8_t* fb);
