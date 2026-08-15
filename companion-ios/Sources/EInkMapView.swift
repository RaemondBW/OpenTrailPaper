import SwiftUI
import MapKit

// The map both the Route and Maps screens draw on: standard Apple Maps, with
// the areas this phone or the device holds drawn over it as hexagons — green
// with a check where the DEVICE has them too, a quiet hairline where they are
// only on the phone.
//
// It used to paint each downloaded area in the head unit's own 1-bit style.
// That is gone; see EInkTileStore for why. The question this screen answers is
// which ground is covered and whether the device has it, and hexagons answer it
// without decoding a byte of map geometry.
//
// UIKit rather than SwiftUI's Map: overlays at this count need MapKit's own
// compositing to stay pinned frame-for-frame while panning. A SwiftUI Canvas
// layered over a Map redraws a frame late and visibly swims.

// MARK: - What the map draws

/// One area of coverage: this phone holds it, the device holds it, or both.
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

/// A mesh node to drop on the map. Kept as a flat value rather than passing
/// MeshNode in, so the map layer stays ignorant of what a mesh is.
struct MeshNodePin: Identifiable, Equatable {
    let id: UInt32
    let label: String          // shown on the callout
    let detail: String         // second callout line: signal, age, precision
    let coordinate: CLLocationCoordinate2D
    /// The sender deliberately blurred this position. Drawn hollow, because a
    /// solid pin on a coordinate the sender rounded off would be a lie.
    let imprecise: Bool

    static func == (a: Self, b: Self) -> Bool {
        a.id == b.id && a.label == b.label && a.detail == b.detail &&
        a.imprecise == b.imprecise &&
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
    var outlines: [OutlineHex] = []
    var selection: [SelectionHex] = []
    var route: MKPolyline? = nil
    var destination: MapDestination? = nil
    var meshNodes: [MeshNodePin] = []
    var camera: MapCameraCommand? = nil
    /// Only turned on once location has actually been granted. MapKit asks for
    /// location the moment it's told to show the blue dot, which would make
    /// merely opening a tab raise a permission prompt out of nowhere — the ask
    /// belongs to the tutorial, the recenter button and Settings, where it comes
    /// with an explanation.
    var showsUserLocation = false
    var showsTrackingButton = false
    /// How much of the map's bottom edge is covered by floating UI. The map
    /// itself runs full-bleed under the card — stopping it above one leaves a
    /// dead band of page background — so this is what keeps the map from
    /// *acting* full height: framing insets by it, and MapKit's own legal link
    /// and tracking button move up out from under the card.
    var bottomInset: CGFloat = 0
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
        if map.layoutMargins.bottom != bottomInset {
            map.preservesSuperviewLayoutMargins = false
            map.layoutMargins.bottom = bottomInset
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
        if c.meshNodes != meshNodes {
            c.meshNodes = meshNodes
            map.annotations.compactMap { $0 as? MeshNodeAnnotation }.forEach(map.removeAnnotation)
            for n in meshNodes { map.addAnnotation(MeshNodeAnnotation(pin: n)) }
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
        for o in outlines { h.combine(o.id); h.combine(o.synced); h.combine(o.missing) }
        for s in selection { h.combine(s.id); h.combine(String(describing: s.kind)) }
        if let r = route { h.combine(ObjectIdentifier(r)) }
        return h.finalize()
    }

    private func rebuildContent(_ map: MKMapView, coordinator c: Coordinator) {
        // The hex layer is updated IN PLACE and survives this teardown, so only
        // the rects that actually changed repaint. During a download areas land
        // several times a second and re-adding the overlay would discard every
        // tile MapKit has already drawn.
        map.removeOverlays(map.overlays.filter { !($0 is HexLayerOverlay) })
        map.removeAnnotations(map.annotations.compactMap { $0 as? SyncedCheckAnnotation })

        // The green check is a per-area badge and only says anything while the
        // areas are big enough to hold one. Framing a region's worth of coverage
        // puts hundreds on screen, overlapping into a solid mat of green. Past
        // this many, the hexagon's own green edge carries it alone.
        let syncedCount = outlines.filter(\.synced).count
        let showChecks = syncedCount <= 120

        var flat: [HexLayerOverlay.Hex] = []
        flat.reserveCapacity(selection.count + outlines.count)
        // Selection sits UNDER the coverage hexes: ground already downloaded
        // should read as downloaded, not as a pending selection tint.
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
            map.addOverlay(o, level: .aboveRoads)
            return o
        }()
        for rect in hexLayer.update(flat) {
            c.hexRenderer?.setNeedsDisplay(rect)
        }
        // Last, so the route always sits on top of the coverage.
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
        // Frame into the part of the map that isn't under the floating card, so
        // "fit all my coverage" doesn't fit half of it behind the controls.
        let pad = UIEdgeInsets(top: 0, left: 0, bottom: bottomInset, right: 0)
        switch command.target {
        case .region(let r):
            stopFollowing()
            // setRegion has no edgePadding, so go through the rect form once
            // there is something to inset for.
            if bottomInset > 0 {
                map.setVisibleMapRect(Self.mapRect(r), edgePadding: pad, animated: true)
            } else {
                map.setRegion(r, animated: true)
            }
        case .rect(let rect):
            stopFollowing()
            map.setVisibleMapRect(rect, edgePadding: pad, animated: true)
        case .followUser:
            map.setUserTrackingMode(.follow, animated: true)
        }
    }

    private static func mapRect(_ r: MKCoordinateRegion) -> MKMapRect {
        let a = MKMapPoint(CLLocationCoordinate2D(
            latitude: r.center.latitude + r.span.latitudeDelta / 2,
            longitude: r.center.longitude - r.span.longitudeDelta / 2))
        let b = MKMapPoint(CLLocationCoordinate2D(
            latitude: r.center.latitude - r.span.latitudeDelta / 2,
            longitude: r.center.longitude + r.span.longitudeDelta / 2))
        return MKMapRect(x: min(a.x, b.x), y: min(a.y, b.y),
                         width: abs(a.x - b.x), height: abs(a.y - b.y))
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    final class Coordinator: NSObject, MKMapViewDelegate, UIGestureRecognizerDelegate {
        var onTap: ((CLLocationCoordinate2D) -> Void)?
        var onLongPress: ((CLLocationCoordinate2D) -> Void)?
        var onRegionChange: ((MKCoordinateRegion) -> Void)?
        var contentKey = 0                // 0 is never a real signature here
        fileprivate var hexes: HexLayerOverlay?
        fileprivate weak var hexRenderer: HexLayerRenderer?
        var route: MKPolyline?
        var destination: MapDestination?
        var meshNodes: [MeshNodePin] = []
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
            if let node = annotation as? MeshNodeAnnotation {
                let id = "meshnode"
                let v = (map.dequeueReusableAnnotationView(withIdentifier: id) as? MKMarkerAnnotationView)
                    ?? MKMarkerAnnotationView(annotation: node, reuseIdentifier: id)
                v.annotation = node
                v.canShowCallout = true
                v.glyphImage = UIImage(systemName: "dot.radiowaves.left.and.right")
                // A blurred position is drawn as an outline, not a solid pin: the
                // sender rounded the coordinate off on purpose and the map should
                // not claim more than they gave.
                if node.pin.imprecise {
                    v.markerTintColor = UIColor(Palette.faint)
                    v.glyphTintColor = UIColor(Palette.ink)
                } else {
                    v.markerTintColor = UIColor(Palette.good)
                    v.glyphTintColor = .white
                }
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

private final class MeshNodeAnnotation: NSObject, MKAnnotation {
    let pin: MeshNodePin
    let title: String?
    let subtitle: String?
    var coordinate: CLLocationCoordinate2D { pin.coordinate }
    init(pin: MeshNodePin) {
        self.pin = pin
        title = pin.label
        subtitle = pin.detail
    }
}

private final class DestinationAnnotation: NSObject, MKAnnotation {
    let title: String?
    let coordinate: CLLocationCoordinate2D
    init(name: String, coordinate: CLLocationCoordinate2D) {
        title = name
        self.coordinate = coordinate
    }
}

// MARK: - The hex layer

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
        ctx.setLineWidth(2.0 / Double(zoomScale))
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
        // These now CARRY the coverage message rather than sitting under an
        // opaque e-ink area, so they are drawn to be seen. At 0.14 over
        // Palette.faint they were tuned to whisper beneath the paper fill, and
        // with that gone they vanished into Apple's green terrain — visible
        // only where they happened to cross water.
        case .downloadedOutline(let synced):
            let c = UIColor(synced ? Palette.good : Palette.muted)
            return (c.withAlphaComponent(synced ? 0.30 : 0.22), c)
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
