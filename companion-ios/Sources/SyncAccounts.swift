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
// Uploads: Strava takes the FIT straight from the phone with the user's bearer
// token; RideWithGPS goes through the service, because every RWGPS request
// also needs the API key.
//
// Only this app: every call to the service carries a Firebase App Check token
// (App Attest on a device; a registered debug token on the simulator), and
// the sign-in return is a Universal Link on the service's host that iOS hands
// to this app alone. The service refuses anything else.

enum SyncProvider: String, CaseIterable, Identifiable {
    case strava, ridewithgps
    var id: String { rawValue }
    var title: String { self == .strava ? "Strava" : "RideWithGPS" }
}

struct SyncTokens: Codable, Equatable {
    var accessToken: String
    var refreshToken: String?
    var expiresAt: TimeInterval?     // unix seconds; nil = does not expire
    var athleteName: String?
    var athleteId: Int?

    var expiresSoon: Bool {
        guard let expiresAt else { return false }
        return Date().timeIntervalSince1970 > expiresAt - 300
    }
}

enum SyncError: LocalizedError {
    case notConfigured, notConnected, cancelled, denied(String), exchange(String), http(Int, String), upload(String)
    var errorDescription: String? {
        switch self {
        case .notConfigured: return "This build has no sync service configured."
        case .notConnected:  return "Not connected."
        case .cancelled:     return "Cancelled."
        case .denied(let e): return "Access was not granted (\(e))."
        case .exchange(let e): return "Sign-in failed: \(e)"
        case .http(let code, let body): return "Server error \(code): \(body.prefix(120))"
        case .upload(let e): return e
        }
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

    override init() {
        super.init()
        for p in SyncProvider.allCases {
            if let t = Keychain.read(p) { tokens[p] = t }
        }
    }

    func isConnected(_ p: SyncProvider) -> Bool { tokens[p] != nil }

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
                    if (err as? ASWebAuthenticationSessionError)?.code != .canceledLogin {
                        self.lastError = err.localizedDescription
                    }
                    return
                }
                if let cb { await self.handle(callback: cb) } else { self.busy = nil }
            }
        }
        // The service sends the user back on https://<host>/app/sync/<p> - a
        // Universal Link. iOS 17.4+ lets the auth session catch that itself;
        // earlier systems get the service's page, whose button reopens the app
        // on the custom scheme (still safe: redeeming needs App Check).
        let s: ASWebAuthenticationSession
        if #available(iOS 17.4, *), let host = base.host {
            s = ASWebAuthenticationSession(url: url, callback: .https(host: host, path: Self.returnPathPrefix + p.rawValue),
                                           completionHandler: done)
        } else {
            s = ASWebAuthenticationSession(url: url, callbackURLScheme: Self.callbackScheme, completionHandler: done)
        }
        s.presentationContextProvider = self
        // Not ephemeral: an existing Strava web login saves typing a password.
        s.prefersEphemeralWebBrowserSession = false
        session = s
        s.start()
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
            store(t, for: p)
        } catch {
            lastError = error.localizedDescription
        }
    }

    func disconnect(_ p: SyncProvider) {
        let t = tokens[p]
        store(nil, for: p)
        // Best effort: tell Strava too, so the app disappears from its list.
        if p == .strava, let t {
            Task { _ = try? await post("v1/auth/strava/revoke", json: ["access_token": t.accessToken]) }
        }
    }

    // MARK: tokens

    /// A bearer token that is good for at least a few minutes.
    func accessToken(for p: SyncProvider) async throws -> String {
        guard var t = tokens[p] else { throw SyncError.notConnected }
        if t.expiresSoon, p == .strava, let r = t.refreshToken {
            let json = try await post("v1/auth/strava/refresh", json: ["refresh_token": r])
            guard let a = json["access_token"] as? String else { throw SyncError.exchange("refresh failed") }
            t.accessToken = a
            t.refreshToken = (json["refresh_token"] as? String) ?? r
            t.expiresAt = (json["expires_at"] as? NSNumber)?.doubleValue
            store(t, for: p)
        }
        return t.accessToken
    }

    private func store(_ t: SyncTokens?, for p: SyncProvider) {
        if let t { Keychain.write(t, for: p); tokens[p] = t } else { Keychain.delete(p); tokens[p] = nil }
    }

    // MARK: uploads

    /// Uploads a .fit. Returns a short status line for the UI ("Uploaded" with
    /// a link when the provider says so).
    func upload(_ fileURL: URL, to p: SyncProvider, name: String) async throws -> String {
        let token = try await accessToken(for: p)
        let data = try Data(contentsOf: fileURL)
        switch p {
        case .strava:
            var form = Multipart()
            form.field("data_type", "fit")
            form.field("name", name)
            form.file("file", filename: fileURL.lastPathComponent, mime: "application/octet-stream", data: data)
            var req = URLRequest(url: URL(string: "https://www.strava.com/api/v3/uploads")!)
            req.httpMethod = "POST"
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
            form.apply(to: &req)
            let json = try await send(req)
            if let e = json["error"] as? String, !e.isEmpty { throw SyncError.upload(e) }
            // Strava processes asynchronously: poll briefly for the activity id.
            guard let id = (json["id"] as? NSNumber)?.int64Value else { return "Uploaded to Strava." }
            for _ in 0..<8 {
                try await Task.sleep(nanoseconds: 1_500_000_000)
                var poll = URLRequest(url: URL(string: "https://www.strava.com/api/v3/uploads/\(id)")!)
                poll.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
                let st = try await send(poll)
                if let e = st["error"] as? String, !e.isEmpty { throw SyncError.upload(e) }
                if let act = (st["activity_id"] as? NSNumber)?.int64Value {
                    return "Uploaded: strava.com/activities/\(act)"
                }
            }
            return "Uploaded to Strava (still processing)."
        case .ridewithgps:
            guard let base = Self.serviceURL else { throw SyncError.notConfigured }
            var form = Multipart()
            form.field("trip[name]", name)
            form.file("file", filename: fileURL.lastPathComponent, mime: "application/octet-stream", data: data)
            var req = URLRequest(url: base.appendingPathComponent("v1/rwgps/trips"))
            req.httpMethod = "POST"
            req.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
            form.apply(to: &req)
            let json = try await send(req)
            if let trip = json["trip"] as? [String: Any], let id = (trip["id"] as? NSNumber)?.int64Value {
                return "Uploaded: ridewithgps.com/trips/\(id)"
            }
            return "Uploaded to RideWithGPS."
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

    private func send(_ req: URLRequest) async throws -> [String: Any] {
        var req = req
        req.setValue("application/json", forHTTPHeaderField: "Accept")
        req.timeoutInterval = 60
        // Prove this is the app: only calls to our own service get the token.
        if let host = req.url?.host, host == Self.serviceURL?.host {
            if Self.attested {
                let t = try await AppCheck.appCheck().token(forcingRefresh: false)
                req.setValue(t.token, forHTTPHeaderField: "X-Firebase-AppCheck")
            }
        }
        let (data, resp) = try await URLSession.shared.data(for: req)
        let code = (resp as? HTTPURLResponse)?.statusCode ?? 0
        let json = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any] ?? [:]
        guard (200..<300).contains(code) else {
            if code == 401 { throw SyncError.upload("Signed out by the provider. Disconnect and connect again.") }
            let msg = (json["error"] as? String) ?? (json["message"] as? String) ?? String(decoding: data, as: UTF8.self)
            throw SyncError.http(code, msg)
        }
        return json
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

struct SyncAccountsCard: View {
    @ObservedObject private var sync = SyncAccounts.shared

    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                Text("Accounts").trackedLabel()
                if SyncAccounts.isConfigured {
                    Text("Connect once, then upload rides from a ride's page. Only your own sign-in is kept, on this phone.")
                        .font(.system(size: 12)).foregroundStyle(Palette.muted)
                    ForEach(SyncProvider.allCases) { p in
                        row(p)
                    }
                    if let e = sync.lastError {
                        Text(e).font(.system(size: 12)).foregroundStyle(Palette.accentDark)
                    }
                } else {
                    Text("Strava and RideWithGPS uploads need the sync service, which this build was not pointed at. The share button still works.")
                        .font(.system(size: 12)).foregroundStyle(Palette.muted)
                }
                if SyncAccounts.isConfigured && !SyncAccounts.attested {
                    Text("This build has no App Check identity, so the service will refuse it.")
                        .font(.system(size: 12)).foregroundStyle(Palette.accentDark)
                }
            }
        }
    }

    @ViewBuilder private func row(_ p: SyncProvider) -> some View {
        HStack(spacing: 12) {
            VStack(alignment: .leading, spacing: 2) {
                Text(p.title).font(BarlowFont.text(15, .semibold)).foregroundStyle(Palette.ink)
                Text(summary(p)).font(.system(size: 12)).foregroundStyle(Palette.muted)
            }
            Spacer()
            if sync.busy == p {
                ProgressView()
            } else if sync.isConnected(p) {
                Button("Disconnect") { sync.disconnect(p) }
                    .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.muted)
            } else {
                Button("Connect") { sync.connect(p) }
                    .font(BarlowFont.text(14, .semibold)).foregroundStyle(Palette.accent)
            }
        }
    }

    private func summary(_ p: SyncProvider) -> String {
        guard let t = sync.tokens[p] else { return "Not connected" }
        return t.athleteName.map { "Connected as \($0)" } ?? "Connected"
    }
}

// MARK: - Upload buttons (ride page)

struct SyncUploadButtons: View {
    let fileURL: URL
    let name: String
    @ObservedObject private var sync = SyncAccounts.shared
    @State private var uploading: SyncProvider?
    @State private var status: [SyncProvider: String] = [:]

    var body: some View {
        let connected = SyncProvider.allCases.filter { sync.isConnected($0) }
        if !connected.isEmpty {
            VStack(spacing: 10) {
                ForEach(connected) { p in
                    PrimaryButton(title: uploading == p ? "Uploading to \(p.title)…" : "Upload to \(p.title)",
                                  systemImage: "arrow.up.circle",
                                  enabled: uploading == nil && status[p] == nil) {
                        upload(p)
                    }
                    if let s = status[p] {
                        Text(s).font(.system(size: 12))
                            .foregroundStyle(s.hasPrefix("Uploaded") ? Palette.good : Palette.accentDark)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        }
    }

    private func upload(_ p: SyncProvider) {
        uploading = p
        Task {
            do { status[p] = try await sync.upload(fileURL, to: p, name: name) }
            catch { status[p] = error.localizedDescription }
            uploading = nil
        }
    }
}
