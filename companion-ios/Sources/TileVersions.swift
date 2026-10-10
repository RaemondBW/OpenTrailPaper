import Foundation
import Combine

/// What this phone sent to each device, per hex: the version record of the
/// tile (.ebm) and of its .poi — "cdn:<hash>" for a file from the pre-built
/// set, "phone:<secs>" for one built here from Overpass (PrebuiltTiles
/// .cdnRecord / .phoneRecord). Written when the device acknowledges the save,
/// keyed by device identity (the peripheral UUID, like the map-layer store),
/// and compared with the CDN index to tell current hexes from updates
/// (docs/prebuilt-tiles.md#versions).
///
/// A JSON file in Application Support rather than UserDefaults: a device can
/// hold thousands of hexes, and the file is rewritten at most once a second.
final class DeviceTileVersions {
    struct Store: Codable, Equatable {
        var tiles: [String: [String: String]] = [:]
        var pois: [String: [String: String]] = [:]
    }

    private(set) var store = Store()
    let url: URL
    private var savePending = false

    init(url: URL = DeviceTileVersions.defaultURL) {
        self.url = url
        if let d = try? Data(contentsOf: url), let s = try? JSONDecoder().decode(Store.self, from: d) {
            store = s
        }
    }

    static var defaultURL: URL {
        let fm = FileManager.default
        let base = (try? fm.url(for: .applicationSupportDirectory, in: .userDomainMask,
                                appropriateFor: nil, create: true))
            ?? fm.urls(for: .documentDirectory, in: .userDomainMask)[0]
        return base.appendingPathComponent("device-tile-versions.json")
    }

    func tiles(_ device: String?) -> [String: String] { device.flatMap { store.tiles[$0] } ?? [:] }
    func pois(_ device: String?) -> [String: String] { device.flatMap { store.pois[$0] } ?? [:] }
    func tile(_ device: String?, _ id: String) -> String? { device.flatMap { store.tiles[$0]?[id] } }
    func poi(_ device: String?, _ id: String) -> String? { device.flatMap { store.pois[$0]?[id] } }

    func setTile(_ device: String?, _ id: String, _ record: String) {
        guard let device else { return }
        store.tiles[device, default: [:]][id] = record
        saveSoon()
    }
    func setPoi(_ device: String?, _ id: String, _ record: String) {
        guard let device else { return }
        store.pois[device, default: [:]][id] = record
        saveSoon()
    }

    /// Forget hexes the device no longer lists (deleted on the card, so a
    /// later copy from elsewhere is not mistaken for ours).
    func pruneTiles(_ device: String?, keeping ids: Set<String>) {
        guard let device, let m = store.tiles[device] else { return }
        let kept = m.filter { ids.contains($0.key) }
        if kept.count != m.count { store.tiles[device] = kept; saveSoon() }
    }
    func prunePois(_ device: String?, keeping ids: Set<String>) {
        guard let device, let m = store.pois[device] else { return }
        let kept = m.filter { ids.contains($0.key) }
        if kept.count != m.count { store.pois[device] = kept; saveSoon() }
    }

    func save() {
        guard let d = try? JSONEncoder().encode(store) else { return }
        try? d.write(to: url, options: .atomic)
    }

    /// Coalesce the per-tile writes of a download into one save a second.
    private func saveSoon() {
        guard !savePending else { return }
        savePending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in
            self?.savePending = false
            self?.save()
        }
    }
}

/// The CDN's group indexes for the hexes on screen, for the Maps screen's
/// "update available" state and the selection's data date. Backed by
/// PrebuiltTiles.IndexCache (an hour, index.json's max-age) and meta.json
/// (OSM snapshot per fragment, also an hour). When the CDN cannot be reached
/// it knows nothing for five minutes, and every hex falls back to the
/// heuristic.
@MainActor final class CdnVersions: ObservableObject {
    static let shared = CdnVersions()

    /// Bumped whenever what is known changes, so views can redraw.
    @Published private(set) var revision = 0
    let base: URL?
    private var groups: [String: PrebuiltTiles.Index] = [:]
    private var groupsAt: [String: Date] = [:]
    private var inFlight: Set<String> = []
    private var meta: [String: Date] = [:]
    private var metaAt: Date?
    private var failedAt: Date?
    static let retryAfter: TimeInterval = 300
    /// Groups looked up per call: a zoomed-out view over a big download
    /// should not fetch a country's worth of indexes at once.
    static let maxGroups = 64

    init(base: URL? = PrebuiltTiles.configuredBase) { self.base = base }

    /// The index entry for `id`, if its group's index is known.
    func entry(_ id: String) -> PrebuiltTiles.Entry? { groups[PrebuiltTiles.group(id)]?.cells[id] }

    /// Fetch (in the background) the indexes for `ids` that are not known or
    /// are over an hour old, and meta.json.
    func refresh(_ ids: [String]) {
        Task { await load(ids) }
    }

    func load(_ ids: [String]) async {
        guard let base else { return }
        if let f = failedAt, Date().timeIntervalSince(f) < Self.retryAfter { return }
        let now = Date()
        let want = Array(Set(ids.map(PrebuiltTiles.group)).filter { g in
            !inFlight.contains(g) && (groupsAt[g].map { now.timeIntervalSince($0) >= PrebuiltTiles.IndexCache.ttl } ?? true)
        }.sorted().prefix(Self.maxGroups))
        let needMeta = metaAt.map { now.timeIntervalSince($0) >= PrebuiltTiles.IndexCache.ttl } ?? true
        guard !want.isEmpty || needMeta else { return }
        inFlight.formUnion(want)
        let lookup = PrebuiltTiles.Lookup(base: base)
        let got = await lookup.groupIndexes(for: want)
        let dead = await lookup.dead
        var newMeta: [String: Date]? = nil
        if !dead, needMeta, let u = URL(string: "meta.json", relativeTo: base) {
            let r = await PrebuiltTiles.get(u, kind: "cdn-meta", timeout: PrebuiltTiles.indexTimeout)
            if r.status == 200, let d = r.data { newMeta = PrebuiltTiles.parseMeta(d) }
            else if r.status == 404 { newMeta = [:] }
        }
        inFlight.subtract(want)
        if dead {
            // Unreachable: forget what is known, so nothing claims "current"
            // from an index that may have moved on.
            failedAt = Date()
            groups = [:]; groupsAt = [:]
        } else {
            for g in want { groups[g] = got[g]; groupsAt[g] = Date() }
            // Tried once an hour, answered or not, so a failing meta.json
            // cannot turn the revision bump below into a refetch loop.
            if let m = newMeta { meta = m }
            if needMeta { metaAt = Date() }
        }
        revision += 1
    }

    /// The OSM snapshot the CDN's tiles for `ids` were built from: the oldest
    /// of their groups' fragments. nil until meta.json and the indexes load.
    func dataDate(_ ids: [String]) -> Date? {
        PrebuiltTiles.dataDate(Set(ids.map(PrebuiltTiles.group)).compactMap { groups[$0] }, meta: meta)
    }
}
