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

// Way flags (EBM sub-tile trailer; see docs/mapgen.js). Bits 0-1 are the
// bike-route network level of the way.
enum : uint8_t {
    MAP_WF_ROUTE_MASK = 0x03,   // 0 none, 1 local, 2 regional, 3 national/intl
    MAP_WF_CYCLEWAY = 0x04,     // dedicated cycleway (or path, bicycle=designated)
    MAP_WF_BIKE_LANE = 0x08,    // painted lane / track along a road
};

struct MapPolyline {
    MapFeatureClass cls;
    uint8_t flags;       // MAP_WF_* (0 for fills and for tiles without flags)
    const int16_t* pts;  // x0,y0,x1,y1,... screen px (portrait 540x960)
    int pointCount;
};

// Cycling POIs (the per-tile .poi file). Type ids and flag bits are the on-file
// bytes — keep in step with docs/mapgen.js poiOf().
enum MapPoiType : uint8_t {
    MAP_POI_WATER = 1,       // drinking water
    MAP_POI_TOILETS = 2,
    MAP_POI_REPAIR = 3,      // bicycle repair station (self-service stand)
    MAP_POI_BIKE_SHOP = 4,
};
enum : uint8_t {
    MAP_PF_RESTRICTED = 0x80,   // any type: fee / customers only / seasonal
    // MAP_POI_REPAIR (and PUMP also on MAP_POI_BIKE_SHOP)
    MAP_PF_PUMP = 0x01, MAP_PF_TOOLS = 0x02, MAP_PF_CHAIN_TOOL = 0x04,
    MAP_PF_STAND = 0x08,
    // MAP_POI_BIKE_SHOP
    MAP_PF_REPAIR = 0x02, MAP_PF_RENTAL = 0x04, MAP_PF_RETAIL = 0x08,
    MAP_PF_SECOND_HAND = 0x10, MAP_PF_EBIKE = 0x20,
    // MAP_POI_TOILETS
    MAP_PF_HAS_WATER = 0x01,
};

struct MapPoi {
    int16_t x, y;    // screen px
    uint8_t type;    // MapPoiType
    uint8_t flags;   // MAP_PF_*
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

    // Cycling POIs (water, toilets, repair stands, bike shops) in view, already
    // filtered for the zoom. Drawn as small icons over the roads.
    const MapPoi* pois = nullptr;
    int poiCount = 0;

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

    // The 3-cell data strip's fields (DashField), from the rider's config
    // (`map` line). Filled by ui_dashboard from dash_config; the preview tool
    // sets its own. Defaults match the strip as it always was.
    uint8_t stripFields[3] = {0 /*DF_SPEED*/, 5 /*DF_DISTANCE*/,
                              6 /*DF_RIDE_TIME*/};

    // Screen direction of true north (0 = up; track-up sets -heading).
    float northDeg;
    bool trackUp;

    // A turn-by-turn banner is drawn over the top of the map. The compass sits
    // in the same place, so it moves below the banner rather than under it.
    bool navBannerVisible = false;

    // Position is coming from the connected phone (device GPS has no fix).
    bool phonePosition = false;

    // A map actually covers the current position. When false the map screen
    // shows a "no map here — download it in the app" prompt.
    bool hasMap = true;

    // How many SD tiles were projected this frame (diagnostics/timing), and how
    // many the viewport actually overlapped. wantedTiles > projectedTiles means
    // the frame ran out of tile budget and the outer ones are simply absent.
    int projectedTiles = 0;
    int wantedTiles = 0;
    // The frame was cut short (see map_store::KeepRendering) — it holds the
    // tiles nearest the rider but not the outer ones.
    bool partial = false;
    int tilePolys = 0;          // polys from tiles (before the base blob)
    int clsCount[7] = {0, 0, 0, 0, 0, 0, 0};  // kept polys per class
};

// Compass touch target (tap toggles north-up / track-up)
struct MapCompassZone {
    int cx, cy, r;
};
extern const MapCompassZone kMapCompass;

// Screen centre-Y of the compass. It moves below the turn-by-turn banner while
// navigating, so the hit test in ui_dashboard MUST ask rather than assume — the
// drawn position and the tap target drifting apart is what made the north-up
// button unreachable during navigation.
int mapCompassCy(bool navBannerVisible);

// Touch targets (zoom buttons on the right edge of the map area)
struct MapTouchZones {
    int zoomX, zoomInY, zoomOutY, size;
};
extern const MapTouchZones kMapZoom;

// One zoom button, on its own. Re-projecting the map after a zoom tap takes the
// best part of a second (tiles come off the SD card), and until this existed
// nothing on the panel moved in that time — so a tap that HAD registered was
// indistinguishable from one that had missed, and riders tapped again. Drawing
// the pressed state and painting just this rectangle answers the touch straight
// away; the full redraw then restores it.
void ui_map_draw_zoom_button(bool zoomIn, bool pressed, uint8_t* fb);

// Native-fb-aligned mask (1 byte/fb-byte, 1 = covered) of the current map frame's
// water/park dithered fills — the dense-dark regions where DU ghosting settles.
// Null until the first map render. The settle-clean flashes exactly these bytes.
const uint8_t* ui_map_dither_mask();

void ui_render_map(const MapScreenData& map, const RideState& s, uint8_t* fb);

// Just the map (features + route + rider), full-screen, no chrome. Used as the
// backdrop behind the powered-off screen.
void ui_render_map_features(const MapScreenData& map, const RideState& s,
                            uint8_t* fb);

// Whole active route fitted into the area above the accept sheet, for the
// "Start navigation?" preview.
void ui_render_route_preview(uint8_t* fb);
