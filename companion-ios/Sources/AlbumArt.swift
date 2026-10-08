import Foundation
import MediaPlayer
import UIKit

/// A track as the device's AMS feed names it (media notify 0xA2).
struct MediaTrack: Equatable {
    var title: String
    var artist: String
    var album: String

    var key: String { "\(title)\u{0}\(artist)\u{0}\(album)" }

    /// The firmware cuts each field to 56 bytes (and AMS may already have cut
    /// it shorter), so a long one may be a prefix of the real name.
    static let fieldMax = 56
    static func maybeCut(_ s: String) -> Bool { s.utf8.count >= fieldMax - 4 }

    /// [title\0artist\0album\0] after the 0xA2 opcode.
    init?(packet d: Data) {
        guard d.count >= 1, d[d.startIndex] == 0xA2 else { return nil }
        let fields = d.dropFirst().split(separator: 0, omittingEmptySubsequences: false)
            .map { String(decoding: $0, as: UTF8.self) }
        guard fields.count >= 3 else { return nil }
        title = fields[0]; artist = fields[1]; album = fields[2]
    }

    init(title: String, artist: String, album: String) {
        self.title = title; self.artist = artist; self.album = album
    }
}

/// Album art for the device's MUSIC page.
///
/// On iPhone the device gets now-playing metadata for ANY player straight from
/// iOS (AMS), but AMS carries no artwork. Art comes from here, from two places:
///
///  1. The Music app (`MPMusicPlayerController.systemMusicPlayer`): the cover of
///     the playing item, read locally. Needs media-library access.
///  2. Any other player (Spotify…): iOS shows apps nothing about it, so the
///     device forwards the AMS track (0xA2) and, only if the rider turned on
///     "Look up album art online", the cover is found with Apple's public
///     iTunes Search API. Matches only when artist and album (or title) agree
///     after normalising; no art beats the wrong art.
///
/// Firmware that forwards AMS tracks drives everything from those packets,
/// which arrive as BLE notifications and so also wake the app in the
/// background. Older firmware never sends one; then only the Music app's own
/// change notifications (foreground only) can trigger a send.
@MainActor
final class AlbumArtFeeder: ObservableObject {
    static let onlineKey = "albumArtOnlineLookup"
    static let side = 300   // fits the device's 324 px frame, like Android

    /// Installed by BLEManager: queue art for the device; false if it can't
    /// go right now (no link, OTA running).
    var send: ((_ gray: [UInt8], _ width: Int, _ height: Int) -> Bool)?

    @Published private(set) var libraryAccess: PermissionState = AlbumArtFeeder.libraryState()
    @Published var onlineLookup: Bool = UserDefaults.standard.bool(forKey: AlbumArtFeeder.onlineKey) {
        didSet {
            UserDefaults.standard.set(onlineLookup, forKey: Self.onlineKey)
            if onlineLookup { evaluate() }
        }
    }

    private var active = false
    private var observing = false
    /// What AMS says is playing, from the device. nil = none seen on this link
    /// (older firmware, or AMS not up) -> Music-app fallback.
    private var deviceTrack: MediaTrack?
    /// The track whose art went out on this link — once per track.
    private var sentKey: String?
    private var work: Task<Void, Never>?
    private var workKey: String?
    /// Recently prepared covers, so A -> B -> A doesn't fetch twice.
    private var grayCache: [(key: String, gray: [UInt8])] = []

    // MARK: lifecycle (BLEManager)

    /// On while connected to a device whose dashboard has a music page.
    func setActive(_ on: Bool) {
        guard on != active else { return }
        active = on
        if on {
            observeMusicApp()
            evaluate()
        } else {
            cancelWork()
        }
    }

    /// The link went down: the next device starts with no art from us.
    func linkDown() {
        deviceTrack = nil
        sentKey = nil
        cancelWork()
    }

    /// 0xA2 from the device: AMS has a (new) track.
    func deviceTrackChanged(_ t: MediaTrack) {
        guard t != deviceTrack else { return }
        deviceTrack = t
        evaluate()
    }

    func requestLibraryAccess() {
        MPMediaLibrary.requestAuthorization { _ in
            Task { @MainActor in
                self.libraryAccess = Self.libraryState()
                self.observeMusicApp()
                self.evaluate()
            }
        }
    }

    // MARK: deciding what to send

    private func evaluate() {
        guard active else { return }
        libraryAccess = Self.libraryState()
        if let t = deviceTrack {
            guard !t.title.isEmpty, t.key != sentKey, t.key != workKey else { return }
            start(key: t.key) { [weak self] in await self?.artFor(deviceTrack: t) }
        } else {
            // No AMS track from the device: the Music app alone, while playing.
            // A beat of delay so the device's own AMS title lands first — a
            // title change after the art would drop it as stale.
            guard let item = playingMusicItem(), let art = item.artwork else { return }
            let key = MediaTrack(title: item.title ?? "", artist: item.artist ?? "",
                                 album: item.albumTitle ?? "").key
            guard key != sentKey, key != workKey else { return }
            start(key: key) { [weak self] in
                try? await Task.sleep(nanoseconds: 1_000_000_000)
                guard let self, self.deviceTrack == nil,
                      self.playingMusicItem()?.persistentID == item.persistentID else { return nil }
                return art.image(at: CGSize(width: Self.side, height: Self.side))
                    .flatMap { ArtImage.grayscale($0, side: Self.side) }
            }
        }
    }

    private func start(key: String, _ produce: @escaping @MainActor () async -> [UInt8]?) {
        cancelWork()
        workKey = key
        // A BLE notification wakes the app for ~10 s; ask for the time a
        // network lookup might need.
        var bg = UIBackgroundTaskIdentifier.invalid
        bg = UIApplication.shared.beginBackgroundTask(withName: "album-art") {
            UIApplication.shared.endBackgroundTask(bg)
            bg = .invalid
        }
        work = Task { @MainActor [weak self] in
            defer {
                if bg != .invalid { UIApplication.shared.endBackgroundTask(bg) }
            }
            let gray = await produce()
            guard let self, !Task.isCancelled, self.workKey == key else { return }
            self.workKey = nil
            guard let gray else { return }
            // Still the track the device is showing?
            if let t = self.deviceTrack, t.key != key { return }
            if self.send?(gray, Self.side, Self.side) == true {
                self.sentKey = key
                self.remember(key, gray)
            }
        }
    }

    private func cancelWork() {
        work?.cancel(); work = nil; workKey = nil
    }

    /// Art for the track AMS reports: the Music app's own cover when it is
    /// the one playing it, else (opt-in) the iTunes catalogue.
    private func artFor(deviceTrack t: MediaTrack) async -> [UInt8]? {
        if let g = grayCache.first(where: { $0.key == t.key })?.gray { return g }
        // The Music app can trail AMS by a moment on a track change: look
        // twice before going online (or giving up).
        for attempt in 0..<2 {
            if let item = musicItem(matching: t) {
                if let img = item.artwork?.image(at: CGSize(width: Self.side, height: Self.side)),
                   let g = ArtImage.grayscale(img, side: Self.side) {
                    return g
                }
                // A streamed catalogue track can come without local artwork;
                // its store id names the exact release, no guessing involved.
                if onlineLookup, let id = Int(item.playbackStoreID), id > 0,
                   let url = await ITunesArt.lookup(storeID: id, title: t.title) {
                    return await ITunesArt.gray(from: url, side: Self.side)
                }
                return nil
            }
            if onlineLookup { break }
            if attempt == 0 {
                try? await Task.sleep(nanoseconds: 1_500_000_000)
                if Task.isCancelled || deviceTrack != t { return nil }
            }
        }
        guard onlineLookup, let url = await ITunesArt.search(t) else { return nil }
        guard !Task.isCancelled, deviceTrack == t else { return nil }
        return await ITunesArt.gray(from: url, side: Self.side)
    }

    private func remember(_ key: String, _ gray: [UInt8]) {
        grayCache.removeAll { $0.key == key }
        grayCache.append((key, gray))
        if grayCache.count > 6 { grayCache.removeFirst() }
    }

    // MARK: the Music app

    private static func libraryState() -> PermissionState {
        switch MPMediaLibrary.authorizationStatus() {
        case .authorized: return .granted
        case .denied: return .denied
        case .restricted: return .unavailable
        default: return .notDetermined
        }
    }

    private var player: MPMusicPlayerController? {
        // Touching the system player without the grant gets nothing useful
        // (and prompts nobody), so don't.
        MPMediaLibrary.authorizationStatus() == .authorized ? .systemMusicPlayer : nil
    }

    private func observeMusicApp() {
        guard !observing, let p = player else { return }
        observing = true
        p.beginGeneratingPlaybackNotifications()
        let nc = NotificationCenter.default
        for name in [Notification.Name.MPMusicPlayerControllerNowPlayingItemDidChange,
                     .MPMusicPlayerControllerPlaybackStateDidChange,
                     UIApplication.didBecomeActiveNotification] {
            nc.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.evaluate() }
            }
        }
    }

    private func playingMusicItem() -> MPMediaItem? {
        guard let p = player, p.playbackState == .playing else { return nil }
        return p.nowPlayingItem
    }

    /// The Music app's item, if it is the track AMS reports. The Music app
    /// keeps a paused item around while Spotify plays, so the title has to
    /// agree before its cover is used.
    private func musicItem(matching t: MediaTrack) -> MPMediaItem? {
        guard let item = player?.nowPlayingItem else { return nil }
        guard TrackMatch.same(item.title ?? "", t.title),
              t.artist.isEmpty || TrackMatch.sameArtist(item.artist ?? "", t.artist)
        else { return nil }
        return item
    }
}

/// UIImage -> 8-bit grayscale side x side, the way the Android app makes it:
/// stretched onto white (art with alpha lands on paper, not black), Rec. 601
/// integer luma.
enum ArtImage {
    static func grayscale(_ image: UIImage, side: Int) -> [UInt8]? {
        let fmt = UIGraphicsImageRendererFormat()
        fmt.scale = 1
        fmt.opaque = true
        let size = CGSize(width: side, height: side)
        let drawn = UIGraphicsImageRenderer(size: size, format: fmt).image { ctx in
            UIColor.white.setFill()
            ctx.fill(CGRect(origin: .zero, size: size))
            image.draw(in: CGRect(origin: .zero, size: size))
        }
        guard let cg = drawn.cgImage else { return nil }
        var rgba = [UInt8](repeating: 255, count: side * side * 4)
        let ok: Bool = rgba.withUnsafeMutableBytes { buf in
            guard let c = CGContext(data: buf.baseAddress, width: side, height: side,
                                    bitsPerComponent: 8, bytesPerRow: side * 4,
                                    space: CGColorSpaceCreateDeviceRGB(),
                                    bitmapInfo: CGImageAlphaInfo.noneSkipLast.rawValue)
            else { return false }
            c.draw(cg, in: CGRect(origin: .zero, size: size))
            return true
        }
        guard ok else { return nil }
        var gray = [UInt8](repeating: 0, count: side * side)
        for i in 0..<(side * side) {
            let r = Int(rgba[i * 4]), g = Int(rgba[i * 4 + 1]), b = Int(rgba[i * 4 + 2])
            gray[i] = UInt8((r * 299 + g * 587 + b * 114) / 1000)
        }
        return gray
    }
}

/// Deciding whether two names are the same track/artist/album. Conservative:
/// case, accents, punctuation and bracketed/dashed qualifiers ("(Remastered
/// 2011)", " - Single", "[Deluxe]", "(feat. X)") are ignored; anything else
/// must agree.
enum TrackMatch {
    static func fold(_ s: String) -> String {
        let f = s.folding(options: [.caseInsensitive, .diacriticInsensitive, .widthInsensitive],
                          locale: nil)
            .replacingOccurrences(of: "&", with: " and ")
        let scalars = f.unicodeScalars.map {
            CharacterSet.alphanumerics.contains($0) ? Character($0) : " "
        }
        return String(scalars).split(separator: " ").joined(separator: " ")
    }

    static func core(_ s: String) -> String {
        var t = s.replacingOccurrences(of: "\\s*[\\(\\[][^\\)\\]]*[\\)\\]]", with: "",
                                       options: .regularExpression)
        if let r = t.range(of: " - ") { t = String(t[..<r.lowerBound]) }
        let c = fold(t)
        return c.isEmpty ? fold(s) : c
    }

    /// `device` came from the device, which may have cut it short.
    static func same(_ name: String, _ device: String) -> Bool {
        guard !device.isEmpty else { return false }
        if core(name) == core(device) { return true }
        guard MediaTrack.maybeCut(device) else { return false }
        // A cut name: everything up to its last whole word must be a prefix.
        var words = fold(device).split(separator: " ")
        if words.count > 1 { words.removeLast() }
        let stem = words.joined(separator: " ")
        return stem.count >= 12 && fold(name).hasPrefix(stem)
    }

    /// The first credited artist: Spotify says "A, B", iTunes "A & B".
    static func primary(_ artist: String) -> String {
        let f = " " + fold(artist) + " "
        var cut = f.endIndex
        for sep in [" and ", " feat ", " featuring ", " ft ", " with ", " x ", " vs "] {
            if let r = f.range(of: sep), r.lowerBound < cut { cut = r.lowerBound }
        }
        let comma = artist.firstIndex(of: ",").map { fold(String(artist[..<$0])) }
        let p = String(f[..<cut]).trimmingCharacters(in: .whitespaces)
        if let comma, !comma.isEmpty, comma.count < p.count { return comma }
        return p
    }

    static func sameArtist(_ name: String, _ device: String) -> Bool {
        if same(name, device) { return true }
        let a = primary(name), b = primary(device)
        return !a.isEmpty && a == b
    }
}

/// Covers from Apple's public iTunes Search API, for players iOS hides from
/// apps. Only used with the rider's opt-in: it sends the track's names to
/// Apple. Results (including misses) are cached so a track is asked once.
enum ITunesArt {
    private struct Response: Decodable { let results: [Item] }
    private struct Item: Decodable {
        let artistName: String?
        let collectionName: String?
        let trackName: String?
        let artworkUrl100: String?
    }

    private static let cacheKey = "albumArtLookupCache"
    private static let missTTL: TimeInterval = 3 * 24 * 3600

    /// Cover URL for a track AMS reported, or nil when nothing matches well.
    static func search(_ t: MediaTrack) async -> URL? {
        guard !t.artist.isEmpty else { return nil }   // too little to be sure
        let ck = "s|" + TrackMatch.fold(t.artist) + "|" + TrackMatch.fold(t.album)
            + "|" + (t.album.isEmpty ? TrackMatch.fold(t.title) : "")
        if let hit = cached(ck) { return hit }
        var found: URL?
        if !t.album.isEmpty {
            // The album's cover: artist and album must both agree.
            let items = await query(["term": "\(t.artist) \(t.album)", "entity": "album"])
            let ok = items.filter {
                TrackMatch.sameArtist($0.artistName ?? "", t.artist)
                    && TrackMatch.same($0.collectionName ?? "", t.album)
            }
            // "Album" and "Album (10th Anniversary Edition)" both pass; the
            // edition named exactly as playing has the right cover.
            let exact = TrackMatch.fold(t.album)
            found = pick(ok.filter { TrackMatch.fold($0.collectionName ?? "") == exact }) ?? pick(ok)
        }
        if found == nil, !t.title.isEmpty {
            // By song: title and artist agree, and the album too when known.
            let items = await query(["term": "\(t.artist) \(t.title)", "entity": "song"])
            let ok = items.filter {
                TrackMatch.sameArtist($0.artistName ?? "", t.artist)
                    && TrackMatch.same($0.trackName ?? "", t.title)
                    && (t.album.isEmpty || TrackMatch.same($0.collectionName ?? "", t.album))
            }
            // Without an album, a song on several releases has several covers:
            // only answer when they all name the same release.
            let albums = Set(ok.map { TrackMatch.core($0.collectionName ?? "") })
            if albums.count == 1 { found = pick(ok) }
        }
        store(ck, found)
        return found
    }

    /// Cover URL of one exact catalogue item (the Music app's store id).
    static func lookup(storeID: Int, title: String) async -> URL? {
        let ck = "id|\(storeID)"
        if let hit = cached(ck) { return hit }
        let items = await query(["id": String(storeID)], path: "lookup")
        let found = pick(items.filter { TrackMatch.same($0.trackName ?? "", title) })
        store(ck, found)
        return found
    }

    static func gray(from url: URL, side: Int) async -> [UInt8]? {
        guard let (data, resp) = try? await URLSession.shared.data(from: url),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let img = UIImage(data: data) else { return nil }
        return ArtImage.grayscale(img, side: side)
    }

    private static func pick(_ items: [Item]) -> URL? {
        guard let s = items.first(where: { $0.artworkUrl100 != nil })?.artworkUrl100 else { return nil }
        // The thumbnail URL names its size; ask for one the panel can use.
        return URL(string: s.replacingOccurrences(of: "100x100bb", with: "600x600bb"))
    }

    private static func query(_ params: [String: String], path: String = "search") async -> [Item] {
        var c = URLComponents(string: "https://itunes.apple.com/\(path)")!
        var q = params.map { URLQueryItem(name: $0.key, value: $0.value) }
        q.append(URLQueryItem(name: "country", value: Locale.current.region?.identifier ?? "US"))
        if path == "search" {
            q.append(URLQueryItem(name: "media", value: "music"))
            q.append(URLQueryItem(name: "limit", value: "25"))
        }
        c.queryItems = q
        guard let url = c.url else { return [] }
        var req = URLRequest(url: url)
        req.timeoutInterval = 8
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let r = try? JSONDecoder().decode(Response.self, from: data) else { return [] }
        return r.results
    }

    // Cache: key -> [url or "", time]. A miss expires (the catalogue grows);
    // a hit doesn't.
    /// .some(nil) is a remembered miss; nil means "not asked yet".
    private static func cached(_ key: String) -> URL?? {
        guard let all = UserDefaults.standard.dictionary(forKey: cacheKey),
              let e = all[key] as? [Any], e.count == 2,
              let s = e[0] as? String, let t = e[1] as? Double else { return nil }
        if s.isEmpty {
            return Date().timeIntervalSince1970 - t < missTTL ? .some(nil) : nil
        }
        return .some(URL(string: s))
    }

    private static func store(_ key: String, _ url: URL?) {
        var all = UserDefaults.standard.dictionary(forKey: cacheKey) ?? [:]
        if all.count > 400 { all = [:] }   // crude, but bounded
        all[key] = [url?.absoluteString ?? "", Date().timeIntervalSince1970]
        UserDefaults.standard.set(all, forKey: cacheKey)
    }
}
