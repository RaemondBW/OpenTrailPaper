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
    @Published private(set) var version = 0

    /// Areas we hold data for on disk.
    private(set) var ids: Set<String> = []

    private var loaded: [String: EBM.Tile] = [:]
    private var loading: Set<String> = []
    private var recency: [String] = []          // LRU, most recent last
    private var heldPoints = 0

    /// Roughly 20 MB of geometry (a point is two doubles in a CGPath, plus
    /// overhead). Well past a screenful of res-6 hexes; the LRU only bites when
    /// panning across a large downloaded region.
    private let pointBudget = 1_200_000

    /// Re-read which areas exist on disk. Cheap (one directory listing).
    func refresh() {
        Task {
            let disk = await TileCache.shared.cachedIds()
            guard disk != ids else { return }
            ids = disk
            // Drop geometry for areas that vanished (cache cleared).
            for gone in loaded.keys where !disk.contains(gone) { evict(gone) }
            version += 1
        }
    }

    /// Decoded geometry, or nil if it hasn't been decoded yet.
    func tile(for id: String) -> EBM.Tile? { loaded[id] }

    /// What the map should draw for `region`, split into areas we can render in
    /// the device's style and ones we can only outline.
    ///
    /// `synced` is the device's own tile list, so the two states the user asked
    /// about — downloaded, and downloaded *and* on the device — are decided in
    /// one place. Anything the device has but this phone has no data for still
    /// gets an outline: coverage the user knows about must not vanish just
    /// because the preview can't be drawn.
    func visibleContent(in region: MKCoordinateRegion, synced: Set<String>)
        -> (areas: [EInkArea], outlines: [OutlineHex]) {
        let all = ids.union(synced)
        guard !all.isEmpty, region.span.latitudeDelta > 0 else { return ([], []) }

        // Pad by a hex so an area half off-screen still draws to the edge.
        let padLat = region.span.latitudeDelta / 2 + 0.06
        let padLon = region.span.longitudeDelta / 2 + 0.06
        let s = region.center.latitude - padLat, n = region.center.latitude + padLat
        let w = region.center.longitude - padLon, e = region.center.longitude + padLon

        var near: [(tile: MapTile, distance: Double)] = []
        for id in all {
            guard let t = geometry(id) else { continue }
            guard t.north >= s, t.south <= n, t.east >= w, t.west <= e else { continue }
            let dLat = t.center.latitude - region.center.latitude
            let dLon = t.center.longitude - region.center.longitude
            near.append((t, dLat * dLat + dLon * dLon))
        }
        // Zoomed out far enough to see hundreds of areas, the ink is sub-pixel
        // anyway. Draw the nearest in full and outline the rest rather than
        // silently dropping them.
        near.sort { $0.distance < $1.distance }
        let drawable = near.prefix(previewLimit).map(\.tile)
        let rest = near.dropFirst(previewLimit).map(\.tile)

        ensureLoaded(drawable.filter { ids.contains($0.id) }.map(\.id))

        var areas: [EInkArea] = []
        var outlines: [OutlineHex] = []
        for t in drawable {
            if let geo = loaded[t.id] {
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

    private let previewLimit = 80

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

    /// Ask for these areas to be available to draw. Already-loaded ids are just
    /// marked recently used; the rest are decoded in the background.
    func ensureLoaded(_ wanted: [String]) {
        for id in wanted {
            touch(id)
            guard loaded[id] == nil, !loading.contains(id), ids.contains(id) else { continue }
            loading.insert(id)
            Task.detached(priority: .utility) {
                let blob = await TileCache.shared.displayData(for: id)
                let tile = blob.flatMap { EBM.decode($0) }
                await MainActor.run { self.finishLoad(id, tile) }
            }
        }
    }

    /// Called after a download so freshly built areas draw without a restart.
    func noteDownloaded(_ newIds: [String]) {
        guard !newIds.isEmpty else { return }
        ids.formUnion(newIds)
        // Re-decode rather than trusting a stale in-memory copy: a rebuilt area
        // can legitimately differ from the one we already drew.
        for id in newIds { evict(id) }
        version += 1
    }

    private func finishLoad(_ id: String, _ tile: EBM.Tile?) {
        loading.remove(id)
        guard let tile else {
            // Undecodable (truncated write, format drift): forget it so we
            // don't retry every pan, and so it draws as plain Apple Maps.
            ids.remove(id)
            version += 1
            return
        }
        loaded[id] = tile
        heldPoints += tile.pointCount
        touch(id)
        trim()
        version += 1
    }

    private func touch(_ id: String) {
        if let i = recency.firstIndex(of: id) { recency.remove(at: i) }
        recency.append(id)
    }

    private func evict(_ id: String) {
        if let t = loaded.removeValue(forKey: id) { heldPoints -= t.pointCount }
        recency.removeAll { $0 == id }
    }

    /// Drop least-recently-wanted geometry until back inside the budget. Never
    /// drops the most recent entries, which are what's on screen right now.
    private func trim() {
        var i = 0
        while heldPoints > pointBudget, i < recency.count, recency.count > 4 {
            let victim = recency[i]
            if loaded[victim] != nil { evict(victim) } else { i += 1 }
        }
    }
}
