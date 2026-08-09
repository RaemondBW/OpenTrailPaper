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
    enum Kind { case pending, done, excluded }
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
    /// areas that just came into view.
    var onRegionChange: ((MKCoordinateRegion) -> Void)? = nil

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
        map.removeOverlays(map.overlays)
        map.removeAnnotations(map.annotations.compactMap { $0 as? SyncedCheckAnnotation })

        // Selection sits UNDER the e-ink areas: a hex already downloaded should
        // read as downloaded, not as a pending selection tint.
        for hex in selection {
            var pts = hex.hexagon
            guard pts.count >= 3 else { continue }
            let poly = StyledPolygon(coordinates: &pts, count: pts.count)
            poly.style = .selection(hex.kind)
            map.addOverlay(poly, level: .aboveRoads)
        }
        for hex in outlines {
            var pts = hex.hexagon
            guard pts.count >= 3 else { continue }
            let poly = StyledPolygon(coordinates: &pts, count: pts.count)
            poly.style = hex.missing ? .missingCoverage
                                     : .downloadedOutline(synced: hex.synced)
            map.addOverlay(poly, level: .aboveRoads)
            if hex.synced { map.addAnnotation(SyncedCheckAnnotation(coordinate: hex.center)) }
        }
        // .aboveRoads, NOT .aboveLabels: the paper covers Apple's roads (we draw
        // our own) but its place and street names come back through on top of
        // the screentones. Painting over them left downloaded areas as anonymous
        // patches of ink — you could see the shape of the coverage but not read
        // where it was.
        for area in areas {
            guard let overlay = EInkOverlay(area: area) else { continue }
            map.addOverlay(overlay, level: .aboveRoads)
            if area.synced { map.addAnnotation(SyncedCheckAnnotation(coordinate: area.center)) }
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
        var onRegionChange: ((MKCoordinateRegion) -> Void)?
        var contentKey = 0                // 0 is never a real signature here
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
            onRegionChange?(map.region)
        }

        func mapView(_ map: MKMapView, rendererFor overlay: MKOverlay) -> MKOverlayRenderer {
            if let eink = overlay as? EInkOverlay { return EInkRenderer(overlay: eink) }
            if let poly = overlay as? StyledPolygon {
                let r = MKPolygonRenderer(polygon: poly)
                let (fill, stroke) = poly.style.colors
                r.fillColor = fill
                r.strokeColor = stroke
                r.lineWidth = 1.5
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

private final class StyledPolygon: MKPolygon {
    enum Style {
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
    var style: Style = .selection(.pending)
}

// MARK: - The e-ink overlay

private final class EInkOverlay: NSObject, MKOverlay {
    let area: EInkArea
    let boundingMapRect: MKMapRect
    /// The hexagon in map-point space — the clip that makes a downloaded area
    /// stop exactly at its edge, and lets neighbouring hexes meet seamlessly.
    let hexPath: CGPath
    var coordinate: CLLocationCoordinate2D { area.center }

    init?(area: EInkArea) {
        guard area.hexagon.count >= 3 else { return nil }
        self.area = area
        let path = CGMutablePath()
        var rect = MKMapRect.null
        for (i, c) in area.hexagon.enumerated() {
            let p = MKMapPoint(c)
            let pt = CGPoint(x: p.x, y: p.y)
            if i == 0 { path.move(to: pt) } else { path.addLine(to: pt) }
            rect = rect.union(MKMapRect(origin: p, size: MKMapSize(width: 0, height: 0)))
        }
        path.closeSubpath()
        hexPath = path
        boundingMapRect = rect
        super.init()
    }
}

private final class EInkRenderer: MKOverlayRenderer {
    private let area: EInkArea
    private let hexPath: CGPath

    init(overlay: EInkOverlay) {
        area = overlay.area
        hexPath = overlay.hexPath
        super.init(overlay: overlay)
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
        guard let hex = hexPath.copy(using: &t) else { return }

        ctx.saveGState()
        ctx.addPath(hex)
        ctx.clip()

        ctx.setFillColor(Self.paper)
        ctx.fill(rect(for: overlay.boundingMapRect))

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
        guard let image else { return }
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
