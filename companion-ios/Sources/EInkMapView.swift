import SwiftUI
import MapKit

// The map both the Route and Maps screens draw on: standard Apple Maps
// everywhere, except the areas this phone has downloaded — those are painted as
// the head unit will render them. Black ink on paper, roads by class, water as
// a 75% dot screen, parks as a diagonal hatch, exactly the screentones in
// src/map_view.cpp. Areas the device also has get a green check.
//
// UIKit rather than SwiftUI's Map: only MKOverlayRenderer can draw a patterned
// fill locked to the map, and only MapKit's own overlay compositing keeps it
// pinned frame-for-frame while panning. A SwiftUI Canvas layered over a Map
// redraws a frame late and visibly swims.

// MARK: - What the map draws

/// A downloaded area with geometry ready to draw.
struct EInkArea: Identifiable {
    let id: String                     // H3 cell id
    let hexagon: [CLLocationCoordinate2D]
    let center: CLLocationCoordinate2D
    let synced: Bool                   // the device has it too -> green check
    let tile: EBM.Tile
}

/// A downloaded area we can't paint right now — its geometry is still decoding,
/// it's one of many at a far-out zoom, or the phone no longer holds its tile
/// data (cache cleared, or another phone sent it). Outlined rather than
/// dropped, so coverage the user knows about never just vanishes.
struct OutlineHex: Identifiable {
    let id: String
    let hexagon: [CLLocationCoordinate2D]
    let center: CLLocationCoordinate2D
    let synced: Bool
    /// Inverted meaning: ground with NO downloaded coverage at all. The Route
    /// screen outlines the hexes a planned route crosses that nobody holds, so
    /// the grid appears only where the maps run out.
    var missing = false
}

/// A hex in the area the user is selecting for download.
struct SelectionHex: Identifiable {
    enum Kind: Equatable { case pending, done, excluded }
    let id: String
    let hexagon: [CLLocationCoordinate2D]
    let kind: Kind
}

struct MapDestination: Equatable {
    let name: String
    let coordinate: CLLocationCoordinate2D
    static func == (a: Self, b: Self) -> Bool {
        a.name == b.name &&
        a.coordinate.latitude == b.coordinate.latitude &&
        a.coordinate.longitude == b.coordinate.longitude
    }
}

/// A one-shot camera move. Identity is the token, so re-issuing the same target
/// (tapping "recenter" twice) still moves the map.
struct MapCameraCommand: Equatable {
    enum Target {
        case region(MKCoordinateRegion)
        case rect(MKMapRect)
        case followUser
    }
    let token = UUID()
    let target: Target
    static func == (a: Self, b: Self) -> Bool { a.token == b.token }
}

/// Screen point -> coordinate, for gestures SwiftUI still owns (the drag-a-box
/// selection). Handed the live map view by the coordinator.
@MainActor
final class MapProjection: ObservableObject {
    fileprivate weak var map: MKMapView?
    func coordinate(at point: CGPoint) -> CLLocationCoordinate2D? {
        guard let map, map.bounds.width > 0 else { return nil }
        return map.convert(point, toCoordinateFrom: map)
    }
}

// MARK: - The view

struct EInkMapView: UIViewRepresentable {
    var areas: [EInkArea] = []
    var outlines: [OutlineHex] = []
    var selection: [SelectionHex] = []
    var route: MKPolyline? = nil
    var destination: MapDestination? = nil
    var camera: MapCameraCommand? = nil
    /// Only turned on once location has actually been granted. MapKit asks for
    /// location the moment it's told to show the blue dot, which would make
    /// merely opening a tab raise a permission prompt out of nowhere — the ask
    /// belongs to the tutorial, the recenter button and Settings, where it comes
    /// with an explanation.
    var showsUserLocation = false
    var showsTrackingButton = false
    var projection: MapProjection? = nil
    var onTap: ((CLLocationCoordinate2D) -> Void)? = nil
    var onLongPress: ((CLLocationCoordinate2D) -> Void)? = nil
    /// Fires as the user pans/zooms, so the screen can ask the store for the
    /// areas that just came into view. The map's own width in points comes with
    /// it: what the store has to decide is how big an area lands ON SCREEN, and
    /// a region alone cannot say that.
    var onRegionChange: ((MKCoordinateRegion, CGFloat) -> Void)? = nil

    func makeUIView(context: Context) -> MKMapView {
        let map = MKMapView()
        map.delegate = context.coordinator
        map.showsUserLocation = showsUserLocation
        map.showsCompass = false

        let tap = UITapGestureRecognizer(target: context.coordinator,
                                         action: #selector(Coordinator.handleTap(_:)))
        tap.delegate = context.coordinator
        map.addGestureRecognizer(tap)
        let press = UILongPressGestureRecognizer(target: context.coordinator,
                                                 action: #selector(Coordinator.handleLongPress(_:)))
        press.minimumPressDuration = 0.5
        press.delegate = context.coordinator
        map.addGestureRecognizer(press)

        if showsTrackingButton {
            let button = MKUserTrackingButton(mapView: map)
            button.translatesAutoresizingMaskIntoConstraints = false
            button.backgroundColor = UIColor(Palette.surface)
            button.layer.cornerRadius = 8
            button.layer.borderWidth = 1
            button.layer.borderColor = UIColor(Palette.hairline).cgColor
            map.addSubview(button)
            NSLayoutConstraint.activate([
                button.trailingAnchor.constraint(equalTo: map.safeAreaLayoutGuide.trailingAnchor,
                                                 constant: -16),
                // Clear of the floating header pills on the Maps screen.
                button.topAnchor.constraint(equalTo: map.safeAreaLayoutGuide.topAnchor,
                                            constant: 60),
                button.widthAnchor.constraint(equalToConstant: 40),
                button.heightAnchor.constraint(equalToConstant: 40),
            ])
        }
        return map
    }

    func updateUIView(_ map: MKMapView, context: Context) {
        let c = context.coordinator
        c.onTap = onTap
        c.onLongPress = onLongPress
        c.onRegionChange = onRegionChange
        projection?.map = map
        if map.showsUserLocation != showsUserLocation {
            map.showsUserLocation = showsUserLocation
        }

        // Overlays are expensive to rebuild, and SwiftUI calls this for any
        // state change on the host view (a progress string, a text field). Only
        // touch MapKit when what's actually drawn has changed.
        let key = signature()
        if key != c.contentKey {
            c.contentKey = key
            rebuildContent(map, coordinator: c)
        }
        if c.destination != destination {
            c.destination = destination
            map.annotations.compactMap { $0 as? DestinationAnnotation }.forEach(map.removeAnnotation)
            if let d = destination {
                map.addAnnotation(DestinationAnnotation(name: d.name, coordinate: d.coordinate))
            }
        }
        if let camera, camera != c.lastCamera {
            c.lastCamera = camera
            apply(camera, to: map)
        }
    }

    /// Everything that changes what's drawn, and nothing that doesn't. The route
    /// is in here because overlays at the same level draw in insertion order —
    /// it has to be re-added on top whenever the areas underneath are rebuilt,
    /// or a downloaded area's paper simply paints over it.
    private func signature() -> Int {
        // A HASH, not a concatenated string. This runs on every SwiftUI update
        // of the host view, and building a comma-joined list of several hundred
        // hex ids each time was itself a measurable part of the Maps page's
        // stutter during a large download.
        var h = Hasher()
        for a in areas { h.combine(a.id); h.combine(a.synced) }
        for o in outlines { h.combine(o.id); h.combine(o.synced); h.combine(o.missing) }
        for s in selection { h.combine(s.id); h.combine(String(describing: s.kind)) }
        if let r = route { h.combine(ObjectIdentifier(r)) }
        return h.finalize()
    }

    private func rebuildContent(_ map: MKMapView, coordinator c: Coordinator) {
        // The e-ink layer is updated IN PLACE and survives this teardown. It is
        // the expensive one to render, and adding it back would discard every
        // tile MapKit has already drawn — which, during a download that lands a
        // new area several times a second, is the whole visible map each time.
        map.removeOverlays(map.overlays.filter {
            !($0 is EInkAreasOverlay) && !($0 is HexLayerOverlay)
        })
        map.removeAnnotations(map.annotations.compactMap { $0 as? SyncedCheckAnnotation })

        // The green check is a per-area badge, and it only says anything while
        // areas are big enough to hold one. Framing a region's worth of
        // coverage puts hundreds of them on screen, overlapping into a solid
        // mat of green — hundreds of annotations for MapKit to place, saying
        // less than nothing. The hex edge already draws green when synced, so
        // past this many the edge carries it alone.
        let synced = areas.filter(\.synced).count + outlines.filter(\.synced).count
        let showChecks = synced <= 120

        let eink = c.eink ?? {
            let o = EInkAreasOverlay()
            c.eink = o
            map.addOverlay(o, level: .aboveRoads)
            return o
        }()
        for rect in eink.update(areas) {
            c.einkRenderer?.setNeedsDisplay(rect)
        }

        // Selection and outlines: also ONE overlay, inserted below the e-ink one
        // so a hex already downloaded reads as downloaded rather than as a
        // pending selection tint. (Overlays at one level draw in insertion
        // order, and the e-ink layer is no longer re-added on every change, so
        // "add it last" would no longer put it on top.)
        //
        // These were an MKPolygon apiece, and with a region-sized download that
        // is several hundred overlays torn down and rebuilt every time an area
        // finishes decoding — four times a second through a download. The tiles
        // filled in visibly slowly for that reason, not because decoding is slow
        // (0.22 ms an area at overview detail, measured over 728 of them).
        var flat: [HexLayerOverlay.Hex] = []
        flat.reserveCapacity(selection.count + outlines.count)
        for hex in selection {
            flat.append(.init(id: "s:" + hex.id, hexagon: hex.hexagon,
                              style: .selection(hex.kind)))
        }
        for hex in outlines {
            flat.append(.init(id: "o:" + hex.id, hexagon: hex.hexagon,
                              style: hex.missing ? .missingCoverage
                                                 : .downloadedOutline(synced: hex.synced)))
            if hex.synced && showChecks {
                map.addAnnotation(SyncedCheckAnnotation(coordinate: hex.center))
            }
        }
        let hexLayer = c.hexes ?? {
            let o = HexLayerOverlay()
            c.hexes = o
            map.insertOverlay(o, below: eink)
            return o
        }()
        for rect in hexLayer.update(flat) {
            c.hexRenderer?.setNeedsDisplay(rect)
        }
        // .aboveRoads, NOT .aboveLabels: the paper covers Apple's roads (we draw
        // our own) but its place and street names come back through on top of
        // the screentones. Painting over them left downloaded areas as anonymous
        // patches of ink — you could see the shape of the coverage but not read
        // where it was.
        if showChecks {
            for area in areas where area.synced {
                map.addAnnotation(SyncedCheckAnnotation(coordinate: area.center))
            }
        }
        // Last, so the route always sits on top of the paper.
        if let route { map.addOverlay(route, level: .aboveLabels) }
        c.route = route
    }

    private func apply(_ command: MapCameraCommand, to map: MKMapView) {
        // Only break follow mode if it is actually engaged. Setting the tracking
        // mode at all makes MapKit ask for location authorization, so doing it
        // unconditionally raised a permission prompt the moment a map appeared —
        // before the app had explained anything.
        func stopFollowing() {
            if map.userTrackingMode != .none { map.setUserTrackingMode(.none, animated: false) }
        }
        switch command.target {
        case .region(let r):
            stopFollowing()
            map.setRegion(r, animated: true)
        case .rect(let rect):
            stopFollowing()
            map.setVisibleMapRect(rect, animated: true)
        case .followUser:
            map.setUserTrackingMode(.follow, animated: true)
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject, MKMapViewDelegate, UIGestureRecognizerDelegate {
        var onTap: ((CLLocationCoordinate2D) -> Void)?
        var onLongPress: ((CLLocationCoordinate2D) -> Void)?
        var onRegionChange: ((MKCoordinateRegion, CGFloat) -> Void)?
        var contentKey = 0                // 0 is never a real signature here
        /// The one e-ink overlay, kept across content changes along with the
        /// renderer that holds its drawn tiles.
        fileprivate var eink: EInkAreasOverlay?
        fileprivate weak var einkRenderer: EInkRenderer?
        fileprivate var hexes: HexLayerOverlay?
        fileprivate weak var hexRenderer: HexLayerRenderer?
        var route: MKPolyline?
        var destination: MapDestination?
        var lastCamera: MapCameraCommand?

        @objc func handleTap(_ g: UITapGestureRecognizer) {
            guard let map = g.view as? MKMapView, let onTap else { return }
            onTap(map.convert(g.location(in: map), toCoordinateFrom: map))
        }

        @objc func handleLongPress(_ g: UILongPressGestureRecognizer) {
            guard g.state == .began, let map = g.view as? MKMapView,
                  let onLongPress else { return }
            onLongPress(map.convert(g.location(in: map), toCoordinateFrom: map))
        }

        // Ride alongside MapKit's own pan/zoom rather than swallowing it.
        func gestureRecognizer(_ g: UIGestureRecognizer,
                               shouldRecognizeSimultaneouslyWith other: UIGestureRecognizer) -> Bool {
            true
        }

        func mapView(_ map: MKMapView, regionDidChangeAnimated animated: Bool) {
            onRegionChange?(map.region, map.bounds.width)
        }

        func mapView(_ map: MKMapView, rendererFor overlay: MKOverlay) -> MKOverlayRenderer {
            if let eink = overlay as? EInkAreasOverlay {
                let r = EInkRenderer(overlay: eink)
                einkRenderer = r
                return r
            }
            if let hexes = overlay as? HexLayerOverlay {
                let r = HexLayerRenderer(overlay: hexes)
                hexRenderer = r
                return r
            }
            if let line = overlay as? MKPolyline {
                let r = MKPolylineRenderer(polyline: line)
                r.strokeColor = UIColor(Palette.accent)
                r.lineWidth = 5
                r.lineCap = .round
                return r
            }
            return MKOverlayRenderer(overlay: overlay)
        }

        func mapView(_ map: MKMapView, viewFor annotation: MKAnnotation) -> MKAnnotationView? {
            if annotation is MKUserLocation { return nil }
            if annotation is SyncedCheckAnnotation {
                let id = "synced"
                let v = map.dequeueReusableAnnotationView(withIdentifier: id)
                    ?? MKAnnotationView(annotation: annotation, reuseIdentifier: id)
                v.annotation = annotation
                v.image = SyncedCheckAnnotation.image
                v.canShowCallout = false
                // Never let the checks fight the map for taps — the Maps screen
                // hit-tests hexes underneath them.
                v.isUserInteractionEnabled = false
                return v
            }
            if let dest = annotation as? DestinationAnnotation {
                let id = "destination"
                let v = (map.dequeueReusableAnnotationView(withIdentifier: id) as? MKMarkerAnnotationView)
                    ?? MKMarkerAnnotationView(annotation: dest, reuseIdentifier: id)
                v.annotation = dest
                v.markerTintColor = UIColor(Palette.accent)
                v.canShowCallout = true
                return v
            }
            return nil
        }
    }
}

// MARK: - Annotations

private final class SyncedCheckAnnotation: NSObject, MKAnnotation {
    let coordinate: CLLocationCoordinate2D
    init(coordinate: CLLocationCoordinate2D) { self.coordinate = coordinate }

    /// Drawn rather than tinted from an SF Symbol: `withTintColor` on a symbol
    /// came out plain black here, and the mark has to hold its colour over both
    /// paper and a dark water screentone. The white ring is what keeps it
    /// legible on the dark half.
    /// 14 pt across. It is a status badge on a ~5.6 km hexagon, not a pin: at 22
    /// it crowded the ink underneath and neighbouring checks nearly touched when
    /// zoomed out. The geometry below is proportional, so `d` is the only knob.
    static let image: UIImage = {
        let d: CGFloat = 14
        let k = d / 22                     // the marks were drawn at 22
        return UIGraphicsImageRenderer(size: CGSize(width: d, height: d)).image { _ in
            UIColor.white.setFill()
            UIBezierPath(ovalIn: CGRect(x: 0, y: 0, width: d, height: d)).fill()
            UIColor(Palette.good).setFill()
            let ring = 1.5 * k
            UIBezierPath(ovalIn: CGRect(x: ring, y: ring,
                                        width: d - ring * 2, height: d - ring * 2)).fill()
            let check = UIBezierPath()
            check.move(to: CGPoint(x: 6.2 * k, y: 11.4 * k))
            check.addLine(to: CGPoint(x: 9.6 * k, y: 14.8 * k))
            check.addLine(to: CGPoint(x: 15.8 * k, y: 7.4 * k))
            check.lineWidth = 2.4 * k
            check.lineCapStyle = .round
            check.lineJoinStyle = .round
            UIColor.white.setStroke()
            check.stroke()
        }
    }()
}

private final class DestinationAnnotation: NSObject, MKAnnotation {
    let title: String?
    let coordinate: CLLocationCoordinate2D
    init(name: String, coordinate: CLLocationCoordinate2D) {
        title = name
        self.coordinate = coordinate
    }
}

// MARK: - Flat hex overlays (selection + un-previewable device areas)

/// Every selection and outline hexagon, in ONE overlay — same reasoning as
/// EInkAreasOverlay, and the same in-place update so only what changed repaints.
private final class HexLayerOverlay: NSObject, MKOverlay {
    struct Hex {
        let id: String                 // namespaced: selection and outline can
        let hexagon: [CLLocationCoordinate2D]   // both hold the same H3 id
        let style: HexStyle
    }
    struct Piece {
        let id: String
        let style: HexStyle
        let path: CGPath
        let rect: MKMapRect
    }

    private var storage: [Piece] = []
    private let lock = NSLock()
    var pieces: [Piece] {
        lock.lock()
        defer { lock.unlock() }
        return storage
    }

    let boundingMapRect = MKMapRect.world
    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: 0, longitude: 0) }

    func update(_ hexes: [Hex]) -> [MKMapRect] {
        var previous: [String: Piece] = [:]
        let current = pieces
        previous.reserveCapacity(current.count)
        for p in current { previous[p.id] = p }

        var next: [Piece] = []
        var dirty: [MKMapRect] = []
        var seen = Set<String>()
        next.reserveCapacity(hexes.count)
        for hex in hexes where hex.hexagon.count >= 3 {
            seen.insert(hex.id)
            if let old = previous[hex.id], old.style == hex.style {
                next.append(old)
                continue
            }
            let path = CGMutablePath()
            var rect = MKMapRect.null
            for (i, c) in hex.hexagon.enumerated() {
                let p = MKMapPoint(c)
                let pt = CGPoint(x: p.x, y: p.y)
                if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
                rect = rect.union(MKMapRect(origin: p, size: MKMapSize(width: 0, height: 0)))
            }
            path.closeSubpath()
            next.append(Piece(id: hex.id, style: hex.style, path: path, rect: rect))
            dirty.append(rect)
        }
        for (id, old) in previous where !seen.contains(id) { dirty.append(old.rect) }
        lock.lock()
        storage = next
        lock.unlock()
        return dirty
    }
}

private final class HexLayerRenderer: MKOverlayRenderer {
    private var pieces: [HexLayerOverlay.Piece] { (overlay as! HexLayerOverlay).pieces }

    override func canDraw(_ mapRect: MKMapRect, zoomScale: MKZoomScale) -> Bool {
        pieces.contains { $0.rect.intersects(mapRect) }
    }

    override func draw(_ mapRect: MKMapRect, zoomScale: MKZoomScale, in ctx: CGContext) {
        let o = point(for: MKMapPoint(x: 0, y: 0))
        let probe = point(for: MKMapPoint(x: 1024, y: 1024))
        let scale = (probe.x - o.x) / 1024
        guard scale > 0, zoomScale > 0 else { return }
        var t = CGAffineTransform(translationX: o.x, y: o.y).scaledBy(x: scale, y: scale)
        // Matches what MKPolygonRenderer drew before: 1.5 screen points.
        ctx.setLineWidth(1.5 / Double(zoomScale))
        for piece in pieces where piece.rect.intersects(mapRect) {
            guard let path = piece.path.copy(using: &t) else { continue }
            let (fill, stroke) = piece.style.colors
            ctx.addPath(path)
            ctx.setFillColor(fill.cgColor)
            ctx.setStrokeColor(stroke.cgColor)
            ctx.drawPath(using: .fillStroke)
        }
    }
}

/// Fill and stroke for the flat hexagons. Was nested in the MKPolygon subclass
/// that used to carry one hex each.
enum HexStyle: Equatable {
    case selection(SelectionHex.Kind)
    case downloadedOutline(synced: Bool)
    case missingCoverage

    var colors: (fill: UIColor, stroke: UIColor) {
        switch self {
        case .downloadedOutline(let synced):
            let c = UIColor(synced ? Palette.good : Palette.faint)
            return (c.withAlphaComponent(0.14), c)
        // A gap in the maps, so it has to read as "look here" — the accent,
        // barely tinted: these sit under the route line, which must stay
        // the most legible thing on the screen.
        case .missingCoverage:
            let c = UIColor(Palette.accent)
            return (c.withAlphaComponent(0.10), c.withAlphaComponent(0.7))
        case .selection(.done):
            return (UIColor(Palette.good).withAlphaComponent(0.22), UIColor(Palette.good))
        case .selection(.excluded):
            return (UIColor(Palette.muted).withAlphaComponent(0.08),
                    UIColor(Palette.muted).withAlphaComponent(0.55))
        case .selection(.pending):
            return (UIColor(Palette.accent).withAlphaComponent(0.16), UIColor(Palette.accent))
        }
    }
}

// MARK: - The e-ink overlay

/// EVERY downloaded area, in ONE overlay.
///
/// It used to be one MKOverlay (and one MKOverlayRenderer) per hexagon, and
/// MapKit's per-overlay compositing is what made a screenful expensive — so the
/// store capped drawing at the nearest 24 hexes and outlined the rest, which is
/// why a wide view of a large download was mostly empty outlines with a ragged
/// edge through it. One overlay costs one renderer no matter how much coverage
/// is on screen; the per-frame work is bounded by the rect MapKit asks for, not
/// by how many areas exist.
private final class EInkAreasOverlay: NSObject, MKOverlay {
    struct Piece {
        let area: EInkArea
        /// The hexagon in map-point space — the clip that makes an area stop
        /// exactly at its edge, and lets neighbours meet seamlessly.
        let hexPath: CGPath
        let rect: MKMapRect
    }
    /// Read on MapKit's rendering threads, written on the main actor when the
    /// drawable set changes — so it goes through a lock. The array is copy-on-
    /// write, so a reader takes a cheap snapshot and is unaffected by a swap
    /// half way through its frame.
    private var storage: [Piece] = []
    private let lock = NSLock()
    var pieces: [Piece] {
        lock.lock()
        defer { lock.unlock() }
        return storage
    }

    /// Fixed, and deliberately the whole world. MapKit indexes an overlay by
    /// this rect when it is added, so it cannot be allowed to change as areas
    /// come and go — and the alternative, re-adding the overlay whenever the
    /// set changes, throws away every rendered tile. `canDraw` does the culling
    /// instead, which is cheap and needs no fixed extent.
    let boundingMapRect = MKMapRect.world
    var coordinate: CLLocationCoordinate2D { CLLocationCoordinate2D(latitude: 0, longitude: 0) }

    /// Swap in a new set of areas and report which rects actually changed, so
    /// only those repaint. During a bulk download areas land continuously; with
    /// a blanket invalidation each one would repaint the entire visible map.
    func update(_ areas: [EInkArea]) -> [MKMapRect] {
        let current = pieces
        var previous: [String: Piece] = [:]
        previous.reserveCapacity(current.count)
        for p in current { previous[p.area.id] = p }

        var next: [Piece] = []
        var dirty: [MKMapRect] = []
        var seen = Set<String>()
        next.reserveCapacity(areas.count)
        for area in areas where area.hexagon.count >= 3 {
            seen.insert(area.id)
            if let old = previous[area.id], Self.same(old.area, area) {
                next.append(old)
                continue
            }
            let path = CGMutablePath()
            var rect = MKMapRect.null
            for (i, c) in area.hexagon.enumerated() {
                let p = MKMapPoint(c)
                let pt = CGPoint(x: p.x, y: p.y)
                if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
                rect = rect.union(MKMapRect(origin: p, size: MKMapSize(width: 0, height: 0)))
            }
            path.closeSubpath()
            next.append(Piece(area: area, hexPath: path, rect: rect))
            dirty.append(rect)
        }
        for (id, old) in previous where !seen.contains(id) { dirty.append(old.rect) }
        lock.lock()
        storage = next
        lock.unlock()
        return dirty
    }

    /// Same area, same ink. `pointCount` stands in for the geometry itself:
    /// re-decoding the same blob at the same level of detail gives the same
    /// count, and every reason the geometry actually changes — a rebuilt
    /// download, a switch to overview detail — changes it.
    private static func same(_ a: EInkArea, _ b: EInkArea) -> Bool {
        a.synced == b.synced && a.tile.pointCount == b.tile.pointCount
    }
}

private final class EInkRenderer: MKOverlayRenderer {
    private var areas: EInkAreasOverlay { overlay as! EInkAreasOverlay }
    private var pieces: [EInkAreasOverlay.Piece] { areas.pieces }

    init(overlay: EInkAreasOverlay) {
        super.init(overlay: overlay)
    }

    // The overlay spans the world, so without this MapKit would ask for tiles
    // everywhere. Nothing is drawn where no area lands.
    override func canDraw(_ mapRect: MKMapRect, zoomScale: MKZoomScale) -> Bool {
        pieces.contains { $0.rect.intersects(mapRect) }
    }

    // Device ink: pure black, because the panel's fast refresh is 1-bit and
    // cannot show a grey. Paper matches the app's warm off-white.
    private static let ink = UIColor.black.cgColor
    private static let paper = UIColor(Palette.paper).cgColor

    /// Thin classes first so a motorway crosses on top of a footpath, as on the
    /// device (it draws in the same order).
    private static let drawOrder: [EBM.FeatureClass] =
        [.path, .minor, .tertiary, .secondary, .primary, .arterial]

    override func draw(_ mapRect: MKMapRect, zoomScale: MKZoomScale, in ctx: CGContext) {
        // Map points -> this renderer's drawing space. Linear and rotation-free,
        // so two probes describe it completely, and a prebuilt path (in map
        // points) can be transformed instead of re-projected point by point.
        let o = point(for: MKMapPoint(x: 0, y: 0))
        let probe = point(for: MKMapPoint(x: 1024, y: 1024))
        let scale = (probe.x - o.x) / 1024
        guard scale > 0, zoomScale > 0 else { return }
        var t = CGAffineTransform(translationX: o.x, y: o.y).scaledBy(x: scale, y: scale)

        // Only the areas MapKit is actually asking about. This is what lets the
        // overlay hold a whole region's coverage without the per-frame cost
        // growing with it. One snapshot for the whole frame, so the set cannot
        // change under the loop.
        for piece in pieces where piece.rect.intersects(mapRect) {
            draw(piece, transform: &t, in: ctx, zoomScale: zoomScale,
                 mapRect: mapRect, scale: scale)
        }
    }

    private func draw(_ piece: EInkAreasOverlay.Piece,
                      transform t: inout CGAffineTransform, in ctx: CGContext,
                      zoomScale: MKZoomScale, mapRect: MKMapRect, scale: Double) {
        guard let hex = piece.hexPath.copy(using: &t) else { return }
        let area = piece.area

        ctx.saveGState()
        ctx.addPath(hex)
        ctx.clip()

        ctx.setFillColor(Self.paper)
        ctx.fill(rect(for: piece.rect))

        // The device sheds detail by metres-per-pixel; mirroring that keeps the
        // preview honest AND bounds the work when zoomed out.
        let metersPerPoint = MKMetersPerMapPointAtLatitude(area.center.latitude) / Double(zoomScale)

        // Parks under water: a lake inside a park is water, not parkland.
        if let parks = area.tile.parks, let p = parks.copy(using: &t) {
            screentone(p, image: Self.hatchTone, cellPoints: 6,
                       in: ctx, zoomScale: zoomScale, mapRect: mapRect, scale: scale)
        }
        if let water = area.tile.water, let p = water.copy(using: &t) {
            screentone(p, image: Self.dotTone, cellPoints: 3,
                       in: ctx, zoomScale: zoomScale, mapRect: mapRect, scale: scale)
        }

        ctx.setStrokeColor(Self.ink)
        ctx.setLineCap(.round)
        ctx.setLineJoin(.round)
        for cls in Self.drawOrder {
            guard let width = Self.width(cls, metersPerPoint: metersPerPoint),
                  let path = area.tile.roads[cls], let p = path.copy(using: &t) else { continue }
            ctx.saveGState()
            // A path/trail is a dithered grey line on the panel; a fine dash is
            // the closest thing that still reads as 1-bit here.
            if cls == .path {
                let d = 1.5 / Double(zoomScale)
                ctx.setLineDash(phase: 0, lengths: [d, d])
            }
            ctx.setLineWidth(width / Double(zoomScale))
            ctx.addPath(p)
            ctx.strokePath()
            ctx.restoreGState()
        }
        ctx.restoreGState()

        // Edge last and unclipped, so the full stroke shows: green where the
        // device has this area too, quiet hairline where it's only on the phone.
        ctx.addPath(hex)
        ctx.setStrokeColor(UIColor(area.synced ? Palette.good : Palette.faint).cgColor)
        ctx.setLineWidth((area.synced ? 2.0 : 1.0) / Double(zoomScale))
        ctx.strokePath()
    }

    /// Stroke width in screen points, or nil when the device would shed this
    /// class at this zoom (map_view.cpp styleFor + map_tiles.cpp shedding).
    private static func width(_ cls: EBM.FeatureClass, metersPerPoint mpp: Double) -> Double? {
        switch cls {
        case .path:      return mpp >= 4 ? nil : 1
        case .minor:     return mpp >= 16 ? nil : 2
        case .secondary: return mpp >= 32 ? nil : 3
        case .tertiary:  return mpp >= 32 ? nil : 2
        case .primary:   return mpp >= 16 ? 2 : 4
        case .arterial:  return mpp >= 16 ? 2 : 5
        }
    }

    /// Fills `path` with a 1-bit tone, drawn at a constant size on screen.
    ///
    /// The tile origin is snapped to a global grid in MAP-POINT space, with the
    /// cell quantised to a power of two. Both matter: MapKit hands this renderer
    /// one sub-rect at a time and each downloaded hex is a separate overlay, so
    /// anchoring the pattern to anything local would print a visible seam at
    /// every tile and hex boundary.
    private func screentone(_ path: CGPath, image: CGImage?, cellPoints: Double,
                            in ctx: CGContext, zoomScale: MKZoomScale,
                            mapRect: MKMapRect, scale: Double) {
        // isEmpty as well as the image: clip() with an empty path is a no-op,
        // not an empty clip, so tiling through one covers everything already
        // clipped in — the whole hexagon. See EBM.readFills.
        guard let image, !path.isEmpty else { return }
        let cellMapRaw = cellPoints / Double(zoomScale)
        guard cellMapRaw > 0, cellMapRaw.isFinite else { return }
        let cellMap = pow(2, log2(cellMapRaw).rounded())
        let anchorX = (mapRect.minX / cellMap).rounded(.down) * cellMap
        let anchorY = (mapRect.minY / cellMap).rounded(.down) * cellMap
        let origin = point(for: MKMapPoint(x: anchorX, y: anchorY))
        let cell = cellMap * scale
        guard cell > 0.01 else { return }

        ctx.saveGState()
        ctx.addPath(path)
        ctx.clip()
        ctx.interpolationQuality = .none    // keep the tone hard-edged, not blurred
        ctx.draw(image, in: CGRect(x: origin.x, y: origin.y, width: cell, height: cell),
                 byTiling: true)
        ctx.restoreGState()
    }

    // The device's tones are dots for water and diagonal stripes for parks, and
    // that CHARACTER is what has to survive here — it's how the two read apart
    // at a glance. Their exact ink coverage cannot: on a 235 ppi panel the
    // water's 75% dots integrate into a dark grey, but a phone screentone big
    // enough to still look dotted is coarse enough that 75% is simply black, and
    // the bay swallows the map. So the patterns are kept and the coverage is
    // lightened, preserving the device's ordering — water darker than parkland.
    private static let dotTone = makeTone(2) { x, y in (x & 1) == (y & 1) }        // 50% dots
    private static let hatchTone = makeTone(4) { x, y in ((x - y) & 3) < 1 }       // 25% stripes

    /// A tiny opaque-black-on-clear bitmap, tiled by `screentone`.
    private static func makeTone(_ size: Int, _ on: (Int, Int) -> Bool) -> CGImage? {
        var px = [UInt8](repeating: 0, count: size * size * 4)   // RGBA, premultiplied
        for y in 0..<size {
            for x in 0..<size where on(x, y) {
                let i = (y * size + x) * 4
                px[i] = 0; px[i + 1] = 0; px[i + 2] = 0; px[i + 3] = 255
            }
        }
        guard let provider = CGDataProvider(data: Data(px) as CFData) else { return nil }
        return CGImage(width: size, height: size, bitsPerComponent: 8, bitsPerPixel: 32,
                       bytesPerRow: size * 4, space: CGColorSpaceCreateDeviceRGB(),
                       bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.premultipliedLast.rawValue),
                       provider: provider, decode: nil, shouldInterpolate: false,
                       intent: .defaultIntent)
    }
}
