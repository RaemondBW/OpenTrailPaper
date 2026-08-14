import Foundation
import Combine
import MapKit

/// Holds decoded map geometry for the areas this phone has downloaded, so the
/// map can draw them in the device's own e-ink style.
///
/// Two separate facts, deliberately kept apart:
///   * `ids` — every area we hold tile DATA for. This is what "downloaded"
///     means on the map, and it comes from `TileCache` (the same blobs that get
///     streamed to the device).
///   * `BLEManager.deviceTileIds` — what the DEVICE has. The intersection is
///     what earns a green check.
///
/// Decoding happens off the main actor; the renderer is never allowed to touch
/// the disk, so only tiles already decoded get an overlay. A tile appears the
/// moment its decode lands.
@MainActor
final class EInkTileStore: ObservableObject {
    static let shared = EInkTileStore()

    /// Bumps whenever the drawable set changes, so views re-diff their overlays.
    ///
    /// COALESCED — see bump(). A downloading run finishes tiles continuously and
    /// each one used to publish immediately, so the map rebuilt every overlay it
    /// had, several times a second. With a few hundred hexes on screen that is
    /// O(n) MapKit teardown per tile and the page visibly stutters.
    @Published private(set) var version = 0

    private var bumpPending = false

    /// Publish at most ~4x/sec. Tiles land far faster than that and no rider can
    /// see the difference, but MapKit certainly can.
    private func bump() {
        guard !bumpPending else { return }
        bumpPending = true
        Task { @MainActor in
            try? await Task.sleep(nanoseconds: 250_000_000)
            bumpPending = false
            version &+= 1
        }
    }

    /// Areas we hold data for on disk.
    private(set) var ids: Set<String> = []

    /// Geometry is cached per (area, level of detail): the same hexagon decodes
    /// to a whole street network up close and to a handful of arterials and a
    /// coastline when zoomed out, and a rider zooming in and out wants both to
    /// stay warm rather than re-decoding on every change of direction.
    private struct Key: Hashable {
        let id: String
        let detail: EBM.Detail
    }

    private var loaded: [Key: EBM.Tile] = [:]
    private var loading: Set<Key> = []
    private var recency: [Key] = []             // LRU, most recent last
    private var heldPoints = 0

    /// Roughly 20 MB of geometry (a point is two doubles in a CGPath, plus
    /// overhead). Well past a screenful of res-6 hexes; the LRU only bites when
    /// panning across a large downloaded region.
    ///
    /// It holds for the wide views too, because those decode at `.overview`:
    /// a whole region of areas at a few thousand points each costs less than a
    /// dozen at full street detail.
    private let pointBudget = 1_500_000

    /// Re-read which areas exist on disk. Cheap (one directory listing).
    func refresh() {
        Task {
            let disk = await TileCache.shared.cachedIds()
            guard disk != ids else { return }
            ids = disk
            // Drop geometry for areas that vanished (cache cleared).
            // Array(): evict mutates `loaded`, so iterate a snapshot of the keys.
            for gone in Array(loaded.keys) where !disk.contains(gone.id) { evict(gone) }
            bump()
        }
    }

    /// Decoded geometry, or nil if it hasn't been decoded yet.
    func tile(for id: String, detail: EBM.Detail = .full) -> EBM.Tile? {
        loaded[Key(id: id, detail: detail)]
    }

    /// What the map should draw for `region` on a map `widthPoints` wide, split
    /// into areas we can render in the device's style and ones we can only
    /// outline.
    ///
    /// `synced` is the device's own tile list, so the two states the user asked
    /// about — downloaded, and downloaded *and* on the device — are decided in
    /// one place. Anything the device has but this phone has no data for still
    /// gets an outline: coverage the user knows about must not vanish just
    /// because the preview can't be drawn.
    ///
    /// Whether an area is inked is decided by ZOOM, not by rank. It used to be
    /// the nearest 24 and everything else outlined, which put an arbitrary
    /// ragged line through the middle of the coverage — and moved it on every
    /// pan. Now either the hexes are big enough to read as ink and all of them
    /// are drawn, or they are not and none of them are.
    func visibleContent(in region: MKCoordinateRegion, widthPoints: CGFloat,
                        synced: Set<String>)
        -> (areas: [EInkArea], outlines: [OutlineHex]) {
        let all = ids.union(synced)
        guard !all.isEmpty, region.span.latitudeDelta > 0 else { return ([], []) }

        // Pad by a hex so an area half off-screen still draws to the edge.
        let padLat = region.span.latitudeDelta / 2 + 0.06
        let padLon = region.span.longitudeDelta / 2 + 0.06
        let s = region.center.latitude - padLat, n = region.center.latitude + padLat
        let w = region.center.longitude - padLon, e = region.center.longitude + padLon

        let mpp = metersPerPoint(region, widthPoints)
        let detail: EBM.Detail = mpp >= Self.overviewFrom ? .overview : .full
        let inkable = mpp <= Self.inkAbove

        var near: [(tile: MapTile, distance: Double)] = []
        for id in all {
            guard let t = geometry(id) else { continue }
            guard t.north >= s, t.south <= n, t.east >= w, t.west <= e else { continue }
            let dLat = t.center.latitude - region.center.latitude
            let dLon = t.center.longitude - region.center.longitude
            near.append((t, dLat * dLat + dLon * dLon))
        }
        // Nearest first, so if the safety cap ever bites it is the far edge of
        // the screen that loses its ink, not the middle.
        near.sort { $0.distance < $1.distance }
        let drawable = inkable ? near.prefix(Self.hardLimit).map(\.tile) : []
        let rest = inkable ? near.dropFirst(Self.hardLimit).map(\.tile)
                           : near.map(\.tile)

        ensureLoaded(drawable.filter { ids.contains($0.id) }.map(\.id), detail: detail)

        var areas: [EInkArea] = []
        var outlines: [OutlineHex] = []
        // Whichever level of detail we already hold, preferring the right one.
        // Zooming across the threshold otherwise dropped every area to an
        // outline until its re-decode landed — a full-screen flash of the map
        // disappearing and coming back, every time.
        let fallback: EBM.Detail = detail == .full ? .overview : .full
        for t in drawable {
            if let geo = loaded[Key(id: t.id, detail: detail)]
                      ?? loaded[Key(id: t.id, detail: fallback)] {
                areas.append(EInkArea(id: t.id, hexagon: t.hexagon, center: t.center,
                                      synced: synced.contains(t.id), tile: geo))
            } else {
                outlines.append(OutlineHex(id: t.id, hexagon: t.hexagon, center: t.center,
                                           synced: synced.contains(t.id)))
            }
        }
        for t in rest {
            outlines.append(OutlineHex(id: t.id, hexagon: t.hexagon, center: t.center,
                                       synced: synced.contains(t.id)))
        }
        return (areas, outlines)
    }

    /// Ground metres per screen point at the middle of `region`.
    private func metersPerPoint(_ region: MKCoordinateRegion, _ width: CGFloat) -> Double {
        guard width > 1 else { return .infinity }
        let metres = region.span.longitudeDelta * 111_320
            * cos(region.center.latitude * .pi / 180)
        return metres / Double(width)
    }

    /// Past this, the device itself is drawing arterials and primaries only, so
    /// decoding the rest is memory spent on geometry no pixel shows.
    private static let overviewFrom: Double = 32

    /// Ink while a res-6 hexagon (~5.6 km) is at least ~40 pt across. Beyond
    /// that the roads inside it are thinner than the hairline outline and the
    /// screentones alias into flat grey — an outline says "downloaded" better.
    private static let inkAbove: Double = 5_600 / 40

    /// A backstop, not a design limit. Reaching it means something is framing
    /// more coverage than a phone screen can meaningfully show; the areas past
    /// it still outline, so nothing silently disappears.
    private static let hardLimit = 256

    /// H3 id -> hexagon, memoised. Every id is a fixed cell on the globe, so
    /// this never needs invalidating.
    private var geometryCache: [String: MapTile] = [:]
    private func geometry(_ id: String) -> MapTile? {
        if let t = geometryCache[id] { return t }
        let cell = h3_from_id(id)
        guard cell != 0 else { return nil }
        let t = H3Tiles.tile(from: cell)
        geometryCache[id] = t
        return t
    }

    /// Ask for these areas to be available to draw at `detail`. Already-loaded
    /// ones are just marked recently used; the rest are decoded in the
    /// background.
    func ensureLoaded(_ wanted: [String], detail: EBM.Detail = .full) {
        // Both levels: the one being decoded, and the one standing in for it
        // meanwhile (see the fallback in visibleContent). Evicting the stand-in
        // is what the flash looked like.
        pinned = Set(wanted.flatMap {
            [Key(id: $0, detail: .full), Key(id: $0, detail: .overview)]
        })
        for id in wanted {
            let key = Key(id: id, detail: detail)
            touch(key)
            guard loaded[key] == nil, !loading.contains(key), ids.contains(id) else { continue }
            loading.insert(key)
            Task.detached(priority: .utility) {
                let blob = await TileCache.shared.displayData(for: id)
                let tile = blob.flatMap { EBM.decode($0, detail: detail) }
                await MainActor.run { self.finishLoad(key, tile) }
            }
        }
    }

    /// Called after a download so freshly built areas draw without a restart.
    func noteDownloaded(_ newIds: [String]) {
        guard !newIds.isEmpty else { return }
        ids.formUnion(newIds)
        // Re-decode rather than trusting a stale in-memory copy: a rebuilt area
        // can legitimately differ from the one we already drew. Both levels of
        // detail go, or a zoomed-out map would keep painting the old geometry.
        let stale = Set(newIds)
        for key in Array(loaded.keys) where stale.contains(key.id) { evict(key) }
        bump()
    }

    private func finishLoad(_ key: Key, _ tile: EBM.Tile?) {
        loading.remove(key)
        guard let tile else {
            // Undecodable (truncated write, format drift): forget it so we
            // don't retry every pan, and so it draws as plain Apple Maps.
            ids.remove(key.id)
            bump()
            return
        }
        loaded[key] = tile
        heldPoints += tile.pointCount
        touch(key)
        trim()
        bump()
    }

    private func touch(_ key: Key) {
        if let i = recency.firstIndex(of: key) { recency.remove(at: i) }
        recency.append(key)
    }

    private func evict(_ key: Key) {
        if let t = loaded.removeValue(forKey: key) { heldPoints -= t.pointCount }
        recency.removeAll { $0 == key }
    }

    /// What the map asked for last. Never evicted, however far over budget:
    /// dropping geometry that is on screen only makes it decode again on the
    /// next frame, so a tight budget would turn into a permanent re-decode loop
    /// with the areas flickering between ink and outline.
    private var pinned: Set<Key> = []

    /// Drop least-recently-wanted geometry until back inside the budget.
    private func trim() {
        var i = 0
        while heldPoints > pointBudget, i < recency.count {
            let victim = recency[i]
            if !pinned.contains(victim), loaded[victim] != nil { evict(victim) }
            else { i += 1 }
        }
    }
}
