import Foundation

// Fetches firmware from GitHub Releases at runtime, instead of baking a copy
// into the app.
//
// WHY THIS REPLACED THE BUNDLE. The old scheme shipped Sources/firmware.bin and
// a `bundledFirmwareVersion` string that had to be kept in step with
// src/config.h by hand. Three things had to move together for every firmware
// change, and when they didn't the failure was invisible: the version string
// still matched, the bytes didn't, and `updateAvailable` compares strings — so
// the app would happily "update" a device to a *different* binary wearing the
// same version. That happened more than once. Now there is one source of truth
// (the release), the app carries no firmware, and shipping firmware no longer
// means shipping an app build.
//
// The web flasher can't do this — GitHub serves release assets from a blob host
// with no CORS header, so docs/flash.js has to bundle them server-side. A native
// URLSession has no such restriction, so the app can go straight to the source.
@MainActor
final class FirmwareRelease: ObservableObject {
    static let shared = FirmwareRelease()

    /// Which releases the rider follows. Stable is the newest version release
    /// (`/releases/latest`); nightly is the moving "nightly" prerelease CI
    /// rebuilds on every push to main: newer code, less tested.
    enum Channel: String, CaseIterable {
        case stable, nightly
    }

    struct Release: Equatable {
        let tag: String          // the version it carries: "v1.05", "v1.23-nightly.81dbf91"
        let assetURL: URL        // firmware.bin download
        let size: Int
        let notes: String
    }

    static let channelKey = "firmwareChannel"
    @Published private(set) var channel: Channel =
        Channel(rawValue: UserDefaults.standard.string(forKey: channelKey) ?? "") ?? .stable
    @Published private(set) var latest: Release?
    @Published private(set) var checking = false
    @Published private(set) var error: String?
    /// Download progress while fetching the binary, nil when not downloading.
    @Published private(set) var downloadProgress: Double?

    private let repo = "RaemondBW/OpenTrailPaper"
    private var cached: (tag: String, data: Data)?
    /// The channel changed while a check was out; its answer is for the old one.
    private var recheck = false

    /// Follow `c` from now on. What the other channel offered is dropped at
    /// once, so Install can never fetch a release from the channel just left.
    func setChannel(_ c: Channel) {
        guard c != channel else { return }
        channel = c
        UserDefaults.standard.set(c.rawValue, forKey: Self.channelKey)
        latest = nil
        error = nil
        if checking { recheck = true } else { Task { await check() } }
    }

    /// The version a release carries. A version release is tagged with it; the
    /// nightly's tag never moves, so CI titles it "Firmware <version>".
    nonisolated static func version(tag: String, name: String) -> String {
        guard tag == "nightly" else { return tag }
        let prefix = "Firmware "
        return name.hasPrefix(prefix) ? String(name.dropFirst(prefix.count)) : name
    }

    /// Ask GitHub what the newest release on the channel is. Cheap (one JSON
    /// request) and safe to call on every appearance of the firmware screen.
    func check() async {
        guard !checking else { return }
        checking = true
        error = nil
        defer {
            checking = false
            if recheck { recheck = false; Task { await check() } }
        }

        let ch = channel
        let path = ch == .nightly ? "releases/tags/nightly" : "releases/latest"
        let url = URL(string: "https://api.github.com/repos/\(repo)/\(path)")!
        var req = URLRequest(url: url)
        req.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 15

        do {
            let (data, resp) = try await URLSession.shared.data(for: req)
            guard ch == channel else { return }
            guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
                // 403 here is nearly always the unauthenticated rate limit
                // (60/hour/IP), which is worth naming rather than reporting as
                // a generic failure.
                let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
                error = code == 403 ? "GitHub rate limit — try again shortly"
                      : code == 404 && ch == .nightly ? "No nightly build yet"
                      : "Couldn't reach GitHub (\(code))"
                return
            }
            guard let obj = try JSONSerialization.jsonObject(with: data) as? [String: Any],
                  let tag = obj["tag_name"] as? String,
                  let assets = obj["assets"] as? [[String: Any]],
                  let bin = assets.first(where: { ($0["name"] as? String) == "firmware.bin" }),
                  let urlStr = bin["browser_download_url"] as? String,
                  let assetURL = URL(string: urlStr)
            else {
                error = "That release has no firmware.bin"
                return
            }
            latest = Release(tag: Self.version(tag: tag, name: obj["name"] as? String ?? ""),
                             assetURL: assetURL,
                             size: bin["size"] as? Int ?? 0,
                             notes: (obj["body"] as? String) ?? "")
        } catch {
            if ch == channel { self.error = "Couldn't reach GitHub" }
        }
    }

    /// The firmware image for `release`, downloading it if it isn't already in
    /// hand. Kept in memory only: it is ~1.8 MB, wanted once per update, and
    /// caching it on disk would just be another copy to go stale.
    /// `transferId` names the TransferCenter entry the byte counts go to.
    func image(for release: Release, transferId: String? = nil) async throws -> Data {
        if let c = cached, c.tag == release.tag { return c.data }
        downloadProgress = 0
        defer { downloadProgress = nil }

        var req = URLRequest(url: release.assetURL)
        req.timeoutInterval = 60
        let delegate = transferId.map { TransferProgressDelegate(id: $0, upload: false) }
        let (tmp, resp) = try await URLSession.shared.download(for: req, delegate: delegate)
        guard let http = resp as? HTTPURLResponse, http.statusCode == 200 else {
            throw URLError(.badServerResponse)
        }
        let data = try Data(contentsOf: tmp)
        // A truncated download would be flashed to the device and brick the
        // slot, so check the length the API told us to expect.
        if release.size > 0 && data.count != release.size {
            throw URLError(.dataLengthExceedsMaximum)
        }
        cached = (release.tag, data)
        return data
    }
}
