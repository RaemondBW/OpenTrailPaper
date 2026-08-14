import Foundation
import Combine
import MapKit

/// Which areas this phone has downloaded, so the map can show coverage.
///
/// Two separate facts, deliberately kept apart:
///   * `ids` — every area we hold tile DATA for. This is what "downloaded"
///     means on the map, and it comes from `TileCache` (the same blobs that get
///     streamed to the device).
///   * `BLEManager.deviceTileIds` — what the DEVICE has. The intersection is
///     what earns a green check.
///
/// This used to decode each area's `.ebm` and paint it in the head unit's own
/// 1-bit style — paper, screentoned water and parkland, roads by class. That is
/// gone. It was expensive in a way that could not be designed away: geometry in
/// a CGPath measures ~25 bytes a point, six times what the same point occupies
/// in the tile it came from, so a region-sized download ran to 312 MB at full
/// street detail and still needed three levels of detail, an LRU, a decode
/// queue and a bespoke overlay renderer to stay inside a budget. What the map
/// is actually asked is "which ground do I have, and does the device have it
/// too" — and a hexagon with a check answers that completely.
@MainActor
final class EInkTileStore: ObservableObject {
    static let shared = EInkTileStore()

    /// Bumps whenever the drawable set changes, so views re-diff their overlays.
    ///
    /// COALESCED — see bump(). A downloading run finishes areas continuously and
    /// each one used to publish immediately, so the map rebuilt every overlay it
    /// had, several times a second.
    @Published private(set) var version = 0

    private var bumpPending = false

    /// Publish at most ~4x/sec. Areas land far faster than that and no rider can
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

    /// Re-read which areas exist on disk. Cheap (one directory listing).
    func refresh() {
        Task {
            let disk = await TileCache.shared.cachedIds()
            guard disk != ids else { return }
            ids = disk
            bump()
        }
    }

    /// The hexagons to draw for `region`: everything this phone or the device
    /// holds that the viewport touches, each carrying whether the DEVICE has it.
    ///
    /// Anything the device has but this phone has no data for is included too —
    /// coverage the rider knows about must not vanish because the blob lives
    /// somewhere else.
    func visibleContent(in region: MKCoordinateRegion, synced: Set<String>)
        -> [OutlineHex] {
        let all = ids.union(synced)
        guard !all.isEmpty, region.span.latitudeDelta > 0 else { return [] }

        // Pad by a hex so an area half off-screen still draws to the edge.
        let padLat = region.span.latitudeDelta / 2 + 0.06
        let padLon = region.span.longitudeDelta / 2 + 0.06
        let s = region.center.latitude - padLat, n = region.center.latitude + padLat
        let w = region.center.longitude - padLon, e = region.center.longitude + padLon

        var out: [OutlineHex] = []
        out.reserveCapacity(all.count)
        for id in all {
            guard let t = geometry(id) else { continue }
            guard t.north >= s, t.south <= n, t.east >= w, t.west <= e else { continue }
            out.append(OutlineHex(id: t.id, hexagon: t.hexagon, center: t.center,
                                  synced: synced.contains(t.id)))
        }
        return out
    }

    /// Called after a download so freshly built areas appear without a restart.
    func noteDownloaded(_ newIds: [String]) {
        guard !newIds.isEmpty else { return }
        ids.formUnion(newIds)
        bump()
    }

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
}
