#pragma once

// Map screen renderer (design 1f: 1-bit track-up map). Takes features
// already projected to screen coordinates — the v2 tile loader will
// produce MapScreenData from OSM tiles on the SD card; tools/preview
// feeds it a synthetic scene today.
//
// Same host/device split as ui_render.h: pure pixels, no hardware.

#include <cstdint>

struct RideState;

enum MapFeatureClass : uint8_t {
    MAP_ROAD_MAJOR,   // 40% gray, wide
    MAP_ROAD_MINOR,   // 40% gray, medium
    MAP_PATH,         // 40% gray, dashed thin
    MAP_RAIL,         // gray, long-dashed thin
    MAP_WATER,        // light gray, wide
};

struct MapPolyline {
    MapFeatureClass cls;
    const int16_t* pts;  // x0,y0,x1,y1,... screen px (portrait 540x960)
    int pointCount;
};

struct MapScreenData {
    const MapPolyline* features;
    int featureCount;

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
