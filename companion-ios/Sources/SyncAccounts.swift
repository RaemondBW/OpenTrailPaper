import AuthenticationServices
import FirebaseAppCheck
import FirebaseCore
import Foundation
import Security
import SwiftUI
import UIKit

// Strava and RideWithGPS accounts: connect, keep the tokens, upload rides.
//
// The phone never holds either provider's client secret or the RideWithGPS API
// key. Those live in the sync-auth service (cloud/sync-auth), which runs the
// consent flow: the app opens `<service>/v1/auth/<provider>/start` in an
// ASWebAuthenticationSession, the provider sends its code to the service, the
// service swaps it for tokens and bounces back to
// `opentrailpaper://sync/<provider>?handoff=…&state=…`, and the app trades the
// short-lived handoff for the tokens over HTTPS. Only this user's tokens ever
// reach the phone, and they are kept in the Keychain.
//
// Uploads: Strava and Intervals.icu take the FIT straight from the phone with
// the user's bearer token; RideWithGPS goes through the service, because every
// RWGPS request also needs the API key.
//
// Only this app: every call to the service carries a Firebase App Check token
// (App Attest on a device; a registered debug token on the simulator), and
// the sign-in return is a Universal Link on the service's host that iOS hands
// to this app alone. The service refuses anything else.

enum SyncProvider: String, CaseIterable, Identifiable {
    case strava, intervals, ridewithgps
    var id: String { rawValue }
    var title: String {
        switch self {
        case .strava: return "Strava"
        case .intervals: return "Intervals.icu"
        case .ridewithgps: return "RideWithGPS"
        }
    }
    /// The word on a ride's "✓ STRAVA" tag.
    var shortTitle: String {
        switch self {
        case .strava: return "Strava"
        case .intervals: return "Intervals"
        case .ridewithgps: return "RWGPS"
        }
    }
}

/// Which services each ride has been uploaded to from this phone, by ride
/// file name, so the Rides list can tag them and a ride page shows its tile
/// as done after it is reopened. Kept as one JSON string in UserDefaults under
/// `key`, which views watch with @AppStorage.
enum UploadLog {
    static let key = "rideUploads"

    static func decode(_ raw: String) -> [String: [String]] {
        (try? JSONDecoder().decode([String: [String]].self, from: Data(raw.utf8))) ?? [:]
    }

    static func providers(for name: String, raw: String? = nil) -> [SyncProvider] {
        let all = decode(raw ?? UserDefaults.standard.string(forKey: key) ?? "")
        return (all[name] ?? []).compactMap(SyncProvider.init(rawValue:))
    }

    static func record(_ p: SyncProvider, for name: String) {
        var all = decode(UserDefaults.standard.string(forKey: key) ?? "")
        var list = all[name] ?? []
        if !list.contains(p.rawValue) { list.append(p.rawValue) }
        all[name] = list
        if let data = try? JSONEncoder().encode(all) {
            UserDefaults.standard.set(String(decoding: data, as: UTF8.self), forKey: key)
        }
    }
}

struct SyncTokens: Codable, Equatable {
    var accessToken: String
    var refreshToken: String?
    var expiresAt: TimeInterval?     // unix seconds; nil = does not expire
    var athleteName: String?
    var athleteId: Int?
    /// What the rider granted, as Strava reports it ("read,read_all,..."); nil = not reported.
    var scope: String?

    /// Within `margin` seconds of expiry (or past it). Unix seconds throughout.
    func expiresWithin(_ margin: TimeInterval, now: TimeInterval = Date().timeIntervalSince1970) -> Bool {
        guard let expiresAt else { return false }
        return now >= expiresAt - margin
    }
}

enum SyncError: LocalizedError {
    /// http: a non-2xx answer, its message already worded for the user
    /// (SyncErrors.describe); reason is the service's machine-readable tag.
    case notConfigured, notConnected, cancelled, denied(String), exchange(String),
         http(Int, String, reason: String?), upload(String), signedOut(String)
    var errorDescription: String? {
        switch self {
        case .notConfigured: return "This build has no sync service configured."
        case .notConnected:  return "Not connected."
        case .cancelled:     return "Cancelled."
        case .denied(let e): return "Access was not granted (\(e))."
        case .exchange(let e): return "Sign-in failed: \(e)"
        case .http(_, let message, _): return message
        case .upload(let e): return e
        case .signedOut(let title): return "\(title) no longer accepts this phone's sign-in. Disconnect and connect \(title) again."
        }
    }
}

/// Words a non-2xx answer for the user, without dumping bodies at them (a
/// Cloudflare error document is a cloudflare.com "type" URL and nothing else).
/// Mirrors SyncErrors in the Android app (data/SyncAuth.kt).
enum SyncErrors {
    static let service = "service"

    static func describe(code: Int, body: String, who: String) -> String {
        let detail = detail(body)
        if who == service, let detail {
            if code == 401, detail.lowercased().hasPrefix("app check") {
                return "The sync service could not verify this app (\(detail))."
            }
            return detail
        }
        let from = who == service ? "The sync service" : who
        switch code {
        case 401: return "\(from) refused the sign-in (HTTP 401)."
        case 429: return "\(from) is limiting requests right now. Try again in 15 minutes."
        case 502...504, 520...530: return "\(from) could not be reached (HTTP \(code)). Try again in a few minutes."
        case 500...: return "\(from) had a problem (HTTP \(code)). Try again later."
        default:
            if let detail { return "\(from): \(detail) (HTTP \(code))" }
            return "\(from) refused the request (HTTP \(code))."
        }
    }

    /// The human part of an error body, or nil: never HTML, never a CDN's error document.
    static func detail(_ body: String) -> String? {
        let trimmed = body.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.hasPrefix("{"),
              let obj = (try? JSONSerialization.jsonObject(with: Data(trimmed.utf8))) as? [String: Any] else { return nil }
        if let type = obj["type"] as? String, type.lowercased().contains("cloudflare") { return nil }
        for k in ["error", "message", "detail", "title"] {
            if let v = (obj[k] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines), !v.isEmpty, !v.hasPrefix("http") {
                let line = v.split(separator: "\n", maxSplits: 1).first.map(String.init) ?? v
                return String(line.prefix(160))
            }
        }
        return nil
    }
}

@MainActor
final class SyncAccounts: NSObject, ObservableObject {
    static let shared = SyncAccounts()

    /// Base URL of the sync-auth service. Set per build through the
    /// OTP_SYNC_SERVICE_URL build setting (project.yml) -> Info.plist; it is a
    /// public address, not a secret. Empty = the accounts UI says so.
    static let serviceURL: URL? = {
        guard let s = Bundle.main.object(forInfoDictionaryKey: "SyncServiceURL") as? String,
              let url = URL(string: s.trimmingCharacters(in: .whitespaces)),
              url.scheme?.hasPrefix("http") == true else { return nil }
        return url
    }()
    static var isConfigured: Bool { serviceURL != nil }
    static let callbackScheme = "opentrailpaper"
    static let returnPathPrefix = "/app/sync/"

    /// Firebase (App Check only) from the Info.plist identifiers. Call once at
    /// launch, before anything asks for a token. A build without the ids runs
    /// unattested, which the service rejects; the Accounts card says so.
    static func configureAppCheck() {
        let info = Bundle.main.infoDictionary ?? [:]
        guard let appId = info["FirebaseAppID"] as? String, appId.contains(":"),
              let apiKey = info["FirebaseAPIKey"] as? String, !apiKey.isEmpty,
              let sender = info["FirebaseSenderID"] as? String, !sender.isEmpty else { return }
        let opts = FirebaseOptions(googleAppID: appId, gcmSenderID: sender)
        opts.apiKey = apiKey
        opts.projectID = info["FirebaseProjectID"] as? String
        #if targetEnvironment(simulator)
        // No Secure Enclave: the debug provider prints a token to the console
        // once; register it under App Check > Apps > Manage debug tokens.
        AppCheck.setAppCheckProviderFactory(AppCheckDebugProviderFactory())
        #else
        AppCheck.setAppCheckProviderFactory(AppAttestProviderFactory())
        #endif
        FirebaseApp.configure(options: opts)
        attested = true
        #if targetEnvironment(simulator)
        // Ask once now so the debug token is printed at launch (the provider
        // only logs it on the first request) and cached for the first Connect.
        AppCheck.appCheck().token(forcingRefresh: false) { _, err in
            if let err { print("[sync] app check: \(err.localizedDescription)") }
        }
        #endif
    }
    private(set) static var attested = false

    private final class AppAttestProviderFactory: NSObject, AppCheckProviderFactory {
        func createProvider(with app: FirebaseApp) -> AppCheckProvider? { AppAttestProvider(app: app) }
    }

    @Published private(set) var tokens: [SyncProvider: SyncTokens] = [:]
    @Published private(set) var busy: SyncProvider?
    @Published var lastError: String?

    private var session: ASWebAuthenticationSession?
    private var pendingState: [SyncProvider: String] = [:]
    /// The refresh in flight per account. Strava may rotate the refresh token
    /// on every refresh, so a second, parallel refresh would spend a token the
    /// first just replaced: callers that arrive meanwhile wait for this one.
    private var refreshing: [SyncProvider: Task<SyncTokens, Error>] = [:]
    static let refreshMargin: TimeInterval = 300

    override init() {
        super.init()
        for p in SyncProvider.allCases {
            if let t = Keychain.read(p) { tokens[p] = t }
        }
    }

    func isConnected(_ p: SyncProvider) -> Bool { tokens[p] != nil || Self.demoConnected(p) }

    /// Screenshot hook (`-demo-accounts`): Strava and intervals.icu read as
    /// connected, and the bundled demo ride as already on Strava, so the
    /// Accounts list and a ride's upload grid can be captured. Nothing is
    /// stored and no request can succeed without real tokens.
    static func demoConnected(_ p: SyncProvider) -> Bool {
        ProcessInfo.processInfo.arguments.contains("-demo-accounts") && p != .ridewithgps
    }

    // MARK: connect / disconnect

    func connect(_ p: SyncProvider) {
        guard let base = Self.serviceURL else { lastError = SyncError.notConfigured.localizedDescription; return }
        let state = Self.randomState()
        pendingState[p] = state
        busy = p
        lastError = nil
        Task {
            do {
                // The consent URL is issued only to an attested app (the ticket
                // in it is what /start accepts), so the flow cannot be started
                // by anything but this app.
                let json = try await post("v1/auth/\(p.rawValue)/begin", json: ["state": state])
                guard let s = json["url"] as? String, let url = URL(string: s) else { throw SyncError.exchange("no url") }
                open(url, base: base, provider: p)
            } catch {
                busy = nil
                lastError = error.localizedDescription
            }
        }
    }

    private func open(_ url: URL, base: URL, provider p: SyncProvider) {
        let done: ASWebAuthenticationSession.CompletionHandler = { [weak self] cb, err in
            Task { @MainActor in
                guard let self else { return }
                self.session = nil
                if let err {
                    self.busy = nil
                    let code = (err as? ASWebAuthenticationSessionError)?.code
                    print("[sync] auth session ended: \(err) code=\(String(describing: code))")
                    if code != .canceledLogin {
                        self.lastError = "Sign-in window closed: \(err.localizedDescription)"
                    }
                    return
                }
                if let cb { await self.handle(callback: cb) } else { self.busy = nil }
            }
        }
        // The service sends the user back on https://<host>/app/sync/<p>. In
        // this in-app browser that page immediately continues on the custom
        // scheme, which is the callback the session waits for. (The https
        // callback mode of iOS 17.4 was tried first: it refuses to start on a
        // phone that has not fetched the domain association, silently.) Still
        // safe: redeeming the handoff needs App Check.
        let s = ASWebAuthenticationSession(url: url, callbackURLScheme: Self.callbackScheme, completionHandler: done)
        s.presentationContextProvider = self
        // Not ephemeral: an existing Strava web login saves typing a password.
        s.prefersEphemeralWebBrowserSession = false
        session = s
        if !s.start() {
            session = nil
            busy = nil
            lastError = "Could not open the sign-in window."
        }
    }

    /// The sign-in return: `https://<service host>/app/sync/<provider>?...` (a
    /// Universal Link) or `opentrailpaper://sync/<provider>?...` (the page's
    /// fallback button). The auth session delivers it itself on iOS 17.4+;
    /// onOpenURL covers the rest.
    func handle(callback url: URL) async {
        let p: SyncProvider?
        if url.scheme == Self.callbackScheme, url.host == "sync" {
            p = SyncProvider(rawValue: url.lastPathComponent)
        } else if url.scheme == "https", url.host == Self.serviceURL?.host,
                  url.path.hasPrefix(Self.returnPathPrefix) {
            p = SyncProvider(rawValue: url.lastPathComponent)
        } else {
            p = nil
        }
        guard let p else { return }
        // Reached through onOpenURL (the page's fallback button) while the auth
        // session is still showing that page: dismiss it.
        session?.cancel()
        session = nil
        defer { busy = nil }
        let q = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
        func item(_ n: String) -> String? { q.first { $0.name == n }?.value }
        guard let state = item("state"), state == pendingState[p] else {
            lastError = "Sign-in response did not match this app's request."
            return
        }
        pendingState[p] = nil
        if let e = item("error") { lastError = SyncError.denied(e).localizedDescription; return }
        guard let handoff = item("handoff") else { lastError = "Sign-in response was incomplete."; return }
        do {
            let json = try await post("v1/auth/handoff", json: ["handoff": handoff])
            guard let access = json["access_token"] as? String else { throw SyncError.exchange("no token") }
            var t = SyncTokens(accessToken: access)
            t.refreshToken = json["refresh_token"] as? String
            t.expiresAt = (json["expires_at"] as? NSNumber)?.doubleValue
            if let a = json["athlete"] as? [String: Any] {
                t.athleteName = a["name"] as? String
                t.athleteId = (a["id"] as? NSNumber)?.intValue
            }
            t.scope = json["scope"] as? String
            store(t, for: p)
        } catch {
            lastError = error.localizedDescription
        }
    }

    func disconnect(_ p: SyncProvider) {
        guard let t = tokens[p] else { store(nil, for: p); return }
        // Best effort: tell the provider too, so the app disappears from its
        // list, with a live token so the revoke is not refused for expiry.
        Task {
            let access = (try? await accessToken(for: p)) ?? t.accessToken
            store(nil, for: p)
            switch p {
            case .strava:
                _ = try? await post("v1/auth/strava/revoke", json: ["access_token": access])
            case .intervals:
                var req = URLRequest(url: URL(string: "https://intervals.icu/api/v1/disconnect-app")!)
                req.httpMethod = "DELETE"
                req.setValue("Bearer \(access)", forHTTPHeaderField: "Authorization")
                _ = try? await send(req)
            case .ridewithgps:
                break
            }
        }
    }

    // MARK: tokens

    // Strava access tokens last six hours and each refresh may hand back a new
    // refresh token that replaces the old one. So: refresh a few minutes ahead
    // of expiry, save what comes back before using it, one refresh at a time,
    // and on a 401 refresh once and retry. Intervals.icu tokens never expire;
    // RideWithGPS ones refresh only if RWGPS ever issued a refresh token.

    private func canRefresh(_ p: SyncProvider, _ t: SyncTokens) -> Bool {
        p != .intervals && t.refreshToken != nil
    }

    /// A bearer token that is good for at least a few minutes.
    func accessToken(for p: SyncProvider) async throws -> String {
        guard let t = tokens[p] else { throw SyncError.notConnected }
        if canRefresh(p, t), t.expiresWithin(Self.refreshMargin) {
            return try await refreshed(p, from: t).accessToken
        }
        return t.accessToken
    }

    /// Runs `call` with a token; on a 401 refreshes once and retries, and a
    /// second 401 means the sign-in is gone.
    private func authorized<R>(_ p: SyncProvider, _ call: (String) async throws -> R) async throws -> R {
        let first = try await accessToken(for: p)
        do {
            return try await call(first)
        } catch SyncError.http(let code, _, _) where code == 401 {}
        guard let t = tokens[p] else { throw SyncError.notConnected }
        let second: String
        if t.accessToken != first {
            second = t.accessToken                        // refreshed meanwhile
        } else if canRefresh(p, t) {
            second = try await refreshed(p, from: t).accessToken
        } else {
            throw SyncError.signedOut(p.title)
        }
        do {
            return try await call(second)
        } catch SyncError.http(let code, _, _) where code == 401 {
            throw SyncError.signedOut(p.title)
        }
    }

    /// New tokens from the service (it holds the client secret), saved before
    /// they are returned. Joins a refresh already running for this account.
    private func refreshed(_ p: SyncProvider, from t: SyncTokens) async throws -> SyncTokens {
        if let running = refreshing[p] { return try await running.value }
        let task = Task { () async throws -> SyncTokens in
            guard let r = t.refreshToken else { throw SyncError.signedOut(p.title) }
            let json: [String: Any]
            do {
                json = try await post("v1/auth/\(p.rawValue)/refresh", json: ["refresh_token": r])
            } catch SyncError.http(401, _, let reason) where reason == "refresh_rejected" {
                throw SyncError.signedOut(p.title)
            }
            guard let a = json["access_token"] as? String, !a.isEmpty else {
                throw SyncError.upload("\(p.title) did not renew the sign-in. Try again in a few minutes.")
            }
            var n = tokens[p] ?? t
            n.accessToken = a
            // Rotated: the old refresh token is dead from now on.
            n.refreshToken = (json["refresh_token"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? r
            n.expiresAt = (json["expires_at"] as? NSNumber)?.doubleValue
            store(n, for: p)
            return n
        }
        refreshing[p] = task
        defer { refreshing[p] = nil }
        return try await task.value
    }

    private func store(_ t: SyncTokens?, for p: SyncProvider) {
        if let t { Keychain.write(t, for: p); tokens[p] = t } else { Keychain.delete(p); tokens[p] = nil }
    }

    // MARK: uploads

    /// Uploads a .fit. Returns a short status line for the UI ("Uploaded" with
    /// a link when the provider says so).
    ///
    /// Reported to TransferCenter, so the upload shows on the Lock Screen and
    /// is given background time to finish if the rider locks the phone.
    func upload(_ fileURL: URL, to p: SyncProvider, name: String) async throws -> String {
        let id = "cloud.\(p.rawValue).\(fileURL.lastPathComponent)"
        let size = (try? fileURL.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0
        TransferCenter.shared.begin(id, kind: .cloudUpload, title: "Uploading to \(p.title)",
                                    detail: name, total: Int64(size))
        do {
            let result = try await performUpload(fileURL, to: p, name: name, transferId: id)
            TransferCenter.shared.finish(id, success: true, message: result)
            return result
        } catch {
            TransferCenter.shared.finish(id, success: false, message: error.localizedDescription)
            throw error
        }
    }

    private func performUpload(_ fileURL: URL, to p: SyncProvider, name: String,
                               transferId tid: String) async throws -> String {
        let data = try Data(contentsOf: fileURL)
        switch p {
        case .strava:
            var form = Multipart()
            form.field("data_type", "fit")
            form.field("name", name)
            form.file("file", filename: fileURL.lastPathComponent, mime: "application/octet-stream", data: data)
            let json = try await authorized(p) { token in
                var req = URLRequest(url: URL(string: "https://www.strava.com/api/v3/uploads")!)
                req.httpMethod = "POST"
                req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                form.apply(to: &req)
                return try await send(req, progress: tid)
            }
            if let e = json["error"] as? String, !e.isEmpty { throw SyncError.upload(e) }
            // Strava processes asynchronously: poll briefly for the activity id.
            TransferCenter.shared.update(tid, detail: "Processing on Strava…", indeterminate: true)
            guard let id = (json["id"] as? NSNumber)?.int64Value else { return "Uploaded to Strava." }
            for _ in 0..<8 {
                try await Task.sleep(nanoseconds: 1_500_000_000)
                let st = try await authorized(p) { token in
                    var poll = URLRequest(url: URL(string: "https://www.strava.com/api/v3/uploads/\(id)")!)
                    poll.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                    return try await send(poll)
                }
                if let e = st["error"] as? String, !e.isEmpty { throw SyncError.upload(e) }
                if let act = (st["activity_id"] as? NSNumber)?.int64Value {
                    return "Uploaded: strava.com/activities/\(act)"
                }
            }
            return "Uploaded to Strava (still processing)."
        case .intervals:
            // Athlete "0" is the bearer's own athlete. device_name is what the
            // activity shows as recorded on; external_id lets Intervals spot a
            // re-upload of the same ride (it also matches by content).
            var comps = URLComponents(string: "https://intervals.icu/api/v1/athlete/0/activities")!
            comps.queryItems = [URLQueryItem(name: "name", value: name),
                                URLQueryItem(name: "device_name", value: "OpenTrailPaper"),
                                URLQueryItem(name: "external_id", value: "otp-" + fileURL.lastPathComponent)]
            var form = Multipart()
            form.file("file", filename: fileURL.lastPathComponent, mime: "application/octet-stream", data: data)
            let json = try await authorized(p) { token in
                var req = URLRequest(url: comps.url!)
                req.httpMethod = "POST"
                req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                form.apply(to: &req)
                return try await send(req, progress: tid)
            }
            // 201 created / 200 duplicate; either way activities[0].id (or id).
            let first = (json["activities"] as? [[String: Any]])?.first ?? json
            if let id = first["id"] as? String ?? (first["id"] as? NSNumber).map({ $0.stringValue }) {
                return "Uploaded: intervals.icu/activities/\(id)"
            }
            return "Uploaded to Intervals.icu."
        case .ridewithgps:
            guard let base = Self.serviceURL else { throw SyncError.notConfigured }
            var form = Multipart()
            form.field("trip[name]", name)
            form.file("file", filename: fileURL.lastPathComponent, mime: "application/octet-stream", data: data)
            let json = try await authorized(p) { token in
                var req = URLRequest(url: base.appendingPathComponent("v1/rwgps/trips"))
                req.httpMethod = "POST"
                req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                form.apply(to: &req)
                return try await send(req, progress: tid)
            }
            if let trip = json["trip"] as? [String: Any], let id = (trip["id"] as? NSNumber)?.int64Value {
                return "Uploaded: ridewithgps.com/trips/\(id)"
            }
            return "Uploaded to RideWithGPS."
        }
    }

    // MARK: routes

    /// The accounts that have saved routes to import (Intervals.icu has none).
    static let routeProviders: [SyncProvider] = [.strava, .ridewithgps]

    /// One page (from 1) of the rider's saved routes. Strava straight from the
    /// phone with the bearer token; RideWithGPS through the service, which adds
    /// the API key. Both through `authorized`, so an expired token is refreshed
    /// and a dead sign-in comes back as SyncError.signedOut.
    func routes(_ p: SyncProvider, page: Int) async throws -> CloudRoutePage {
        guard isConnected(p) else { throw SyncError.upload("Connect \(p.title) in Settings first.") }
        switch p {
        case .strava:
            let athlete = try await stravaAthleteId()
            let url = URL(string: "https://www.strava.com/api/v3/athletes/\(athlete)/routes?page=\(page)&per_page=\(CloudRoutes.stravaPageSize)")!
            let data = try await routeCall(p) { try await self.authorizedGet(p, url) }
            return CloudRoutes.parseStrava(data, page: page)
        case .ridewithgps:
            guard let base = Self.serviceURL else { throw SyncError.notConfigured }
            var comps = URLComponents(url: base.appendingPathComponent("v1/rwgps/routes"), resolvingAgainstBaseURL: false)!
            comps.queryItems = [URLQueryItem(name: "page", value: String(page))]
            let data = try await routeCall(p) { try await self.authorizedGet(p, comps.url!) }
            return CloudRoutes.parseRwgps(data)
        case .intervals:
            throw SyncError.upload("Intervals.icu has no saved routes.")
        }
    }

    /// The route as GPX, for GPXImporter.
    func routeGPX(_ p: SyncProvider, route: CloudRoute) async throws -> Data {
        guard isConnected(p) else { throw SyncError.upload("Connect \(p.title) in Settings first.") }
        let id = route.id.addingPercentEncoding(withAllowedCharacters: .alphanumerics) ?? route.id
        let url: URL
        switch p {
        case .strava: url = URL(string: "https://www.strava.com/api/v3/routes/\(id)/export_gpx")!
        case .ridewithgps:
            guard let base = Self.serviceURL else { throw SyncError.notConfigured }
            url = base.appendingPathComponent("v1/rwgps/routes/\(id)/gpx")
        case .intervals: throw SyncError.upload("Intervals.icu has no saved routes.")
        }
        return try await routeCall(p) { try await self.authorizedGet(p, url, accept: "application/gpx+xml") }
    }

    /// Strava lists routes by athlete id, which the sign-in reports; looked up
    /// once (GET /athlete) if it did not.
    private func stravaAthleteId() async throws -> Int {
        if let id = tokens[.strava]?.athleteId { return id }
        let data = try await routeCall(.strava) {
            try await self.authorizedGet(.strava, URL(string: "https://www.strava.com/api/v3/athlete")!)
        }
        let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        guard let id = (json?["id"] as? NSNumber)?.intValue, id > 0 else {
            throw SyncError.upload("Strava did not say which athlete this is.")
        }
        if var t = tokens[.strava] { t.athleteId = id; store(t, for: .strava) }
        return id
    }

    private func authorizedGet(_ p: SyncProvider, _ url: URL, accept: String = "application/json") async throws -> Data {
        try await authorized(p) { token in
            var req = URLRequest(url: url)
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
            return try await sendData(req, accept: accept)
        }
    }

    /// Route-specific wording for what a provider's refusal means here.
    private func routeCall<R>(_ p: SyncProvider, _ call: () async throws -> R) async throws -> R {
        do {
            return try await call()
        } catch SyncError.http(403, _, _) {
            // Strava's answer to a token without the scope a private route needs.
            throw SyncError.upload("\(p.title) did not allow reading routes with this sign-in. Disconnect and connect \(p.title) again in Settings.")
        } catch SyncError.http(404, _, _) {
            throw SyncError.upload("That route is no longer on \(p.title).")
        }
    }

    // MARK: http

    private func post(_ path: String, json body: [String: Any]) async throws -> [String: Any] {
        guard let base = Self.serviceURL else { throw SyncError.notConfigured }
        var req = URLRequest(url: base.appendingPathComponent(path))
        req.httpMethod = "POST"
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try JSONSerialization.data(withJSONObject: body)
        return try await send(req)
    }

    /// `progress` names a TransferCenter entry to report the request body's
    /// upload progress to.
    private func send(_ req: URLRequest, progress: String? = nil) async throws -> [String: Any] {
        let data = try await sendData(req, progress: progress)
        return (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
    }

    /// The body of a 2xx answer as is; anything else thrown as SyncError.http.
    private func sendData(_ req: URLRequest, progress: String? = nil, accept: String = "application/json") async throws -> Data {
        var req = req
        req.setValue(accept, forHTTPHeaderField: "Accept")
        req.timeoutInterval = 60
        // Prove this is the app: only calls to our own service get the token.
        if let host = req.url?.host, host == Self.serviceURL?.host {
            if Self.attested {
                let t = try await AppCheck.appCheck().token(forcingRefresh: false)
                req.setValue(t.token, forHTTPHeaderField: "X-Firebase-AppCheck")
            }
        }
        let (data, resp): (Data, URLResponse)
        if let progress, let body = req.httpBody {
            var r = req
            r.httpBody = nil
            (data, resp) = try await URLSession.shared.upload(
                for: r, from: body, delegate: TransferProgressDelegate(id: progress, upload: true))
        } else {
            (data, resp) = try await URLSession.shared.data(for: req)
        }
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(code) else {
            let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
            let body = String(decoding: data, as: UTF8.self)
            let host = req.url?.host
            print("[sync] \(req.httpMethod ?? "GET") \(host ?? "")\(req.url?.path ?? ""): HTTP \(code) \(body.prefix(300))")
            throw SyncError.http(code, SyncErrors.describe(code: code, body: body, who: Self.who(host)),
                                 reason: json["reason"] as? String)
        }
        return data
    }

    /// Who answered, for error wording: our service, or a provider by name.
    private static func who(_ host: String?) -> String {
        guard let host, host != serviceURL?.host else { return SyncErrors.service }
        if host.hasSuffix("strava.com") { return SyncProvider.strava.title }
        if host.hasSuffix("intervals.icu") { return SyncProvider.intervals.title }
        if host.hasSuffix("ridewithgps.com") { return SyncProvider.ridewithgps.title }
        return host
    }

    private static func randomState() -> String {
        var bytes = [UInt8](repeating: 0, count: 24)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        return Data(bytes).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

extension SyncAccounts: ASWebAuthenticationPresentationContextProviding {
    nonisolated func presentationAnchor(for session: ASWebAuthenticationSession) -> ASPresentationAnchor {
        MainActor.assumeIsolated {
            UIApplication.shared.connectedScenes
                .compactMap { $0 as? UIWindowScene }
                .flatMap(\.windows)
                .first { $0.isKeyWindow } ?? ASPresentationAnchor()
        }
    }
}

// MARK: - Keychain

private enum Keychain {
    static let service = "com.raemond.opentrailpaper.sync"

    static func read(_ p: SyncProvider) -> SyncTokens? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: p.rawValue,
                                kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let data = out as? Data else { return nil }
        return try? JSONDecoder().decode(SyncTokens.self, from: data)
    }

    static func write(_ t: SyncTokens, for p: SyncProvider) {
        guard let data = try? JSONEncoder().encode(t) else { return }
        delete(p)
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: p.rawValue,
                                kSecAttrAccessible as String: kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly,
                                kSecValueData as String: data]
        SecItemAdd(q as CFDictionary, nil)
    }

    static func delete(_ p: SyncProvider) {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword,
                                kSecAttrService as String: service,
                                kSecAttrAccount as String: p.rawValue]
        SecItemDelete(q as CFDictionary)
    }
}

// MARK: - multipart/form-data

private struct Multipart {
    let boundary = "otp-" + UUID().uuidString
    private var body = Data()

    mutating func field(_ name: String, _ value: String) {
        body.append("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"\r\n\r\n\(value)\r\n")
    }

    mutating func file(_ name: String, filename: String, mime: String, data: Data) {
        body.append("--\(boundary)\r\nContent-Disposition: form-data; name=\"\(name)\"; filename=\"\(filename)\"\r\nContent-Type: \(mime)\r\n\r\n")
        body.append(data)
        body.append("\r\n")
    }

    func apply(to req: inout URLRequest) {
        var b = body
        b.append("--\(boundary)--\r\n")
        req.setValue("multipart/form-data; boundary=\(boundary)", forHTTPHeaderField: "Content-Type")
        req.httpBody = b
    }
}

private extension Data {
    mutating func append(_ s: String) { append(s.data(using: .utf8)!) }
}

// MARK: - Settings card

/// Settings › Accounts: one bordered row per service with its logo, then
/// "Powered by Strava" under the group.
struct SyncAccountsCard: View {
    @ObservedObject private var sync = SyncAccounts.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            SettingsSectionHeader(title: "Accounts")
            if SyncAccounts.isConfigured {
                GroupedRows {
                    ForEach(Array(SyncProvider.allCases.enumerated()), id: \.element) { i, p in
                        if i > 0 { GroupedDivider() }
                        row(p)
                    }
                }
                Text("POWERED BY STRAVA")
                    .font(BarlowFont.text(10, .bold)).tracking(2)
                    .foregroundStyle(Palette.ink)
                Text("Connect once, then upload rides from a ride's page. Only your own sign-in is kept, on this phone.")
                    .font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                if let e = sync.lastError {
                    WarningBanner(title: "Account error", message: e)
                }
            } else {
                Card(padding: 12) {
                    Text("Strava and RideWithGPS uploads need the sync service, which this build was not pointed at. The share button still works.")
                        .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                }
            }
            if SyncAccounts.isConfigured && !SyncAccounts.attested {
                WarningBanner(title: "No App Check identity",
                              message: "This build has no App Check identity, so the service will refuse it.")
            }
        }
    }

    @ViewBuilder private func row(_ p: SyncProvider) -> some View {
        HStack(spacing: 12) {
            ProviderLogo(provider: p, size: 32)
            VStack(alignment: .leading, spacing: 1) {
                Text(p.title).font(BarlowFont.text(15, .bold)).foregroundStyle(Palette.ink)
                Text(summary(p)).font(BarlowFont.text(12)).foregroundStyle(Palette.muted)
                    .lineLimit(2)
            }
            Spacer(minLength: 8)
            if sync.busy == p {
                BlockSpinner()
            } else if sync.isConnected(p) {
                Menu {
                    Button("Disconnect \(p.title)", role: .destructive) { sync.disconnect(p) }
                } label: {
                    Text("✓ ON").font(BarlowFont.text(11, .bold)).tracking(1.5)
                        .foregroundStyle(Palette.ink)
                        .frame(minWidth: 44, minHeight: 44, alignment: .trailing)
                }
                .accessibilityLabel("\(p.title) connected. Disconnect")
            } else {
                Button { sync.connect(p) } label: {
                    Text("CONNECT").font(BarlowFont.text(11, .bold)).tracking(1.5)
                        .padding(.horizontal, 10).padding(.vertical, 6)
                        .foregroundStyle(Palette.ink)
                        .einkBorder()
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .accessibilityLabel("Connect \(p.title)")
            }
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
    }

    private func summary(_ p: SyncProvider) -> String {
        if SyncAccounts.demoConnected(p) { return "Connected as Demo Rider" }
        guard let t = sync.tokens[p] else { return "Not connected" }
        return t.athleteName.map { "Connected as \($0)" } ?? "Connected"
    }
}

// MARK: - Upload grid (ride page)

/// One tile per connected account, side by side; tap a tile to upload. A
/// finished upload turns its tile black.
struct SyncUploadButtons: View {
    let fileURL: URL
    let name: String
    @ObservedObject private var sync = SyncAccounts.shared
    @State private var uploading: SyncProvider?
    @State private var status: [SyncProvider: String] = [:]

    var body: some View {
        let connected = SyncProvider.allCases.filter { sync.isConnected($0) }
        if !connected.isEmpty {
            VStack(alignment: .leading, spacing: 8) {
                Text("Upload to").trackedLabel()
                LazyVGrid(columns: Array(repeating: GridItem(.flexible(), spacing: 10),
                                         count: min(connected.count, 3)),
                          spacing: 10) {
                    ForEach(connected) { tile($0) }
                }
                // Full messages go under the grid; a tile only has room for a word.
                ForEach(connected.filter { failed($0) }) { p in
                    let s = status[p] ?? ""
                    Text(s.contains(p.title) ? s : "\(p.title): \(s)").font(BarlowFont.text(12, .semibold))
                        .foregroundStyle(Palette.ink)
                }
            }
            .onAppear {
                if ProcessInfo.processInfo.arguments.contains("-demo-accounts"),
                   fileURL.lastPathComponent == "demo.fit" { status[.strava] = "Uploaded" }
                // Uploaded earlier from this phone: show the tile as done.
                for p in UploadLog.providers(for: fileURL.lastPathComponent) where status[p] == nil {
                    status[p] = "Uploaded"
                }
            }
        }
    }

    private func uploaded(_ p: SyncProvider) -> Bool { status[p]?.hasPrefix("Uploaded") == true }
    private func failed(_ p: SyncProvider) -> Bool { status[p] != nil && !uploaded(p) }

    private func tile(_ p: SyncProvider) -> some View {
        let done = uploaded(p)
        let busy = uploading == p
        return Button { upload(p) } label: {
            VStack(spacing: 6) {
                ProviderLogo(provider: p, size: 40)
                Text(p.title).font(BarlowFont.text(13, .bold))
                    .lineLimit(1).minimumScaleFactor(0.7)
                Group {
                    if busy { Text("UPLOADING…") }
                    else if done { Text("✓ UPLOADED") }
                    else if failed(p) { Text("TRY AGAIN") }
                    else { Text("↑ UPLOAD") }
                }
                .font(BarlowFont.text(11, .bold)).tracking(1.5)
                .lineLimit(1).minimumScaleFactor(0.7)
            }
            .foregroundStyle(done ? Palette.paper : Palette.ink)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 10).padding(.horizontal, 6)
            .background {
                if done { Palette.ink }
                else if busy || failed(p) { HatchedBackground() }
                else { Palette.paper }
            }
            .einkBorder()
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        // Not .disabled: that greys the logo, and a finished upload should read as done.
        .allowsHitTesting(uploading == nil && !done)
        .opacity(uploading != nil && !busy ? 0.5 : 1)
        .accessibilityLabel(done ? "Uploaded to \(p.title)" : busy ? "Uploading to \(p.title)" : "Upload to \(p.title)")
    }

    private func upload(_ p: SyncProvider) {
        uploading = p
        status[p] = nil
        Task {
            do {
                status[p] = try await sync.upload(fileURL, to: p, name: name)
                if uploaded(p) { UploadLog.record(p, for: fileURL.lastPathComponent) }
            }
            catch { status[p] = error.localizedDescription }
            uploading = nil
        }
    }
}

/// The service's logo as a rounded app-icon tile. Ride with GPS and
/// intervals.icu use their published marks as-is (Ride with GPS asks for its
/// colours to be left alone) on a light tile; Strava's chevrons sit on its orange.
struct ProviderLogo: View {
    let provider: SyncProvider
    var size: CGFloat = 44

    var body: some View {
        let shape = Rectangle()
        shape.fill(provider == .strava ? Palette.strava : .white)
            .overlay(shape.strokeBorder(provider == .strava ? .clear : Palette.ink, lineWidth: 2))
            .frame(width: size, height: size)
            .overlay(mark)
            .accessibilityHidden(true)
    }

    @ViewBuilder private var mark: some View {
        switch provider {
        case .strava:
            StravaMark().fill(.white).frame(width: size * 0.56, height: size * 0.56)
        case .intervals:
            Image("logo-intervals").resizable().scaledToFit().frame(width: size * 0.72)
        case .ridewithgps:
            Image("logo-rwgps").resizable().scaledToFit().frame(width: size * 0.74)
        }
    }
}

/// Strava's two chevrons, on a 24-unit grid.
private struct StravaMark: Shape {
    func path(in r: CGRect) -> Path {
        let k = min(r.width, r.height) / 24
        func pt(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: r.minX + x * k, y: r.minY + y * k) }
        var p = Path()
        p.addLines([pt(15.387, 17.944), pt(13.298, 13.828), pt(10.233, 13.828),
                    pt(15.387, 24), pt(20.537, 13.828), pt(17.471, 13.828)])
        p.closeSubpath()
        p.addLines([pt(10.463, 8.229), pt(13.299, 13.827), pt(17.471, 13.827),
                    pt(10.463, 0), pt(3.463, 13.828), pt(7.632, 13.828)])
        p.closeSubpath()
        return p
    }
}
