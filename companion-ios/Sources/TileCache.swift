import Foundation

/// On-disk cache of built `.ebm` tile blobs, keyed by H3 cell id.
///
/// Building a tile is expensive and fragile: an Overpass fetch (retried across
/// public mirrors, paced a second apart, and prone to 504s on busy servers), an
/// elevation fetch, then encoding roads, water, sea rings and parks. All of that
/// was thrown away the moment anything went wrong — a dropped BLE link mid-send
/// cleared the queue, and re-sending meant re-fetching and re-encoding the whole
/// area from scratch.
///
/// Blobs are immutable for a given cell (the encoders are byte-identical across
/// mapgen.js / build_map.py / MapBuilder.swift by design), so caching them by id
/// is safe and needs no invalidation beyond age.
///
/// Stored in Caches: this is reconstructible from the network, so the system is
/// free to evict it under disk pressure, and it must not count against the
/// user's iCloud backup.
actor TileCache {
    static let shared = TileCache()

    /// Tiles older than this are re-fetched, so OSM edits eventually land.
    private let maxAge: TimeInterval = 60 * 60 * 24 * 90   // 90 days
    /// Rough ceiling before the oldest entries are dropped.
    private let maxBytes: Int = 256 * 1024 * 1024

    private let dir: URL

    init() {
        let base = FileManager.default.urls(for: .cachesDirectory,
                                            in: .userDomainMask)[0]
        // Versioned. Bump when a builder change alters what a tile SHOULD
        // contain, so stale blobs cannot mask the fix. v2: tiles built before
        // the padded-coastline fetch have no sea fill — an ocean-only hex was
        // cached blank (elevation alone made it non-empty), and reusing it would
        // look exactly like the bug still being there.
        dir = base.appendingPathComponent("TileCache-v2", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir,
                                                 withIntermediateDirectories: true)
    }

    private func url(_ id: String) -> URL {
        // Ids are hex H3 strings, so they are already filename-safe; guard anyway
        // so a malformed id can never escape the directory.
        let safe = id.filter { $0.isHexDigit }
        return dir.appendingPathComponent("\(safe).ebm")
    }

    /// Cached blob for `id`, or nil if absent or too old.
    func data(for id: String) -> Data? {
        let u = url(id)
        guard let attrs = try? FileManager.default.attributesOfItem(atPath: u.path),
              let modified = attrs[.modificationDate] as? Date,
              Date().timeIntervalSince(modified) < maxAge,
              let d = try? Data(contentsOf: u), !d.isEmpty
        else { return nil }
        return d
    }

    func store(_ data: Data, for id: String) {
        guard !data.isEmpty else { return }
        try? data.write(to: url(id), options: .atomic)
    }

    func store(_ tiles: [(id: String, data: Data)]) {
        for t in tiles { store(t.data, for: t.id) }
    }

    /// Which of `ids` are already built, and which still need fetching.
    func partition(_ ids: [String]) -> (cached: [(id: String, data: Data)],
                                        missing: [String]) {
        var hit: [(id: String, data: Data)] = []
        var miss: [String] = []
        for id in ids {
            if let d = data(for: id) { hit.append((id, d)) } else { miss.append(id) }
        }
        return (hit, miss)
    }

    /// Total bytes held, for the Settings readout.
    func sizeBytes() -> Int {
        guard let files = try? FileManager.default.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.fileSizeKey]) else { return 0 }
        return files.reduce(0) {
            $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        }
    }

    func clear() {
        try? FileManager.default.removeItem(at: dir)
        try? FileManager.default.createDirectory(at: dir,
                                                 withIntermediateDirectories: true)
    }

    /// Drop the oldest entries until the cache is back under `maxBytes`.
    func trim() {
        guard let files = try? FileManager.default.contentsOfDirectory(
            at: dir, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey])
        else { return }
        var total = files.reduce(0) {
            $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
        }
        guard total > maxBytes else { return }
        let byAge = files.sorted {
            let a = (try? $0.resourceValues(forKeys: [.contentModificationDateKey]))?
                .contentModificationDate ?? .distantPast
            let b = (try? $1.resourceValues(forKeys: [.contentModificationDateKey]))?
                .contentModificationDate ?? .distantPast
            return a < b
        }
        for f in byAge where total > maxBytes {
            let sz = (try? f.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
            try? FileManager.default.removeItem(at: f)
            total -= sz
        }
    }
}
