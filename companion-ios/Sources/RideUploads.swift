import SwiftUI
import Security

enum UploadKeychain {
    private static func query(_ service: String) -> [String: Any] {
        [kSecClass as String: kSecClassGenericPassword,
         kSecAttrService as String: "com.raemond.opentrailpaper.uploads",
         kSecAttrAccount as String: service]
    }
    static func read(_ service: String) throws -> String {
        var q = query(service)
        q[kSecReturnData as String] = true
        var result: CFTypeRef?
        let status = SecItemCopyMatching(q as CFDictionary, &result)
        if status == errSecItemNotFound { return "" }
        guard status == errSecSuccess, let data = result as? Data,
              let key = String(data: data, encoding: .utf8) else {
            throw RideUploadError(message: "Could not read the saved API key. Unlock the phone and try again.")
        }
        return key
    }
    static func save(_ key: String, service: String) throws {
        let q = query(service)
        if key.isEmpty {
            let status = SecItemDelete(q as CFDictionary)
            guard status == errSecSuccess || status == errSecItemNotFound else {
                throw RideUploadError(message: "Could not remove the saved API key.")
            }
            return
        }
        let attributes: [String: Any] = [kSecValueData as String: Data(key.utf8),
            kSecAttrAccessible as String: kSecAttrAccessibleWhenUnlockedThisDeviceOnly]
        var status = SecItemUpdate(q as CFDictionary, attributes as CFDictionary)
        if status == errSecItemNotFound {
            status = SecItemAdd(q.merging(attributes) { _, new in new } as CFDictionary, nil)
        }
        guard status == errSecSuccess else {
            throw RideUploadError(message: "Could not save the API key securely. Try again.")
        }
    }
}

@MainActor
final class RideUploads: ObservableObject {
    static let shared = RideUploads()
    let services: [any RideUploadService] = [IntervalsUploadService()]
    @Published private(set) var active = Set<String>()
    @Published private(set) var messages: [String: String] = [:]
    @Published private(set) var revision = 0

    func save(_ key: String, service: String) throws {
        try UploadKeychain.save(key.trimmingCharacters(in: .whitespacesAndNewlines), service: service)
        messages = [:]
        revision += 1
    }

    func actionID(_ service: String, _ file: URL) -> String { service + ":" + file.path }

    func upload(_ service: any RideUploadService, file: URL) {
        let action = actionID(service.id, file)
        guard !active.contains(action) else { return }
        active.insert(action)
        messages[action] = nil
        // Owned here, so dismissing the detail sheet does not lose a receipt or
        // start a second POST while the original request is still in flight.
        Task {
            defer { active.remove(action) }
            do {
                let credential = try UploadKeychain.read(service.id)
                let data = try await Task.detached { try Data(contentsOf: file) }.value
                let receiptKey = "ride-upload.\(service.id).\(rideUploadHash(Data(credential.utf8))).\(rideUploadHash(data))"
                if UserDefaults.standard.data(forKey: receiptKey) != nil {
                    messages[action] = "Already uploaded to \(service.name)."
                    return
                }
                let receipt = try await service.upload(data, credential: credential)
                UserDefaults.standard.set(try JSONEncoder().encode(receipt), forKey: receiptKey)
                messages[action] = receipt.alreadyUploaded
                    ? "Already uploaded to \(service.name)." : "Uploaded to \(service.name)."
            } catch {
                messages[action] = (error as? RideUploadError)?.message ?? "Could not read the saved ride. Download it again and retry."
            }
        }
    }
}

struct UploadServicesView: View {
    @ObservedObject private var uploads = RideUploads.shared
    @Environment(\.dismiss) private var dismiss
    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text("Upload services").font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
                    Text("Connect your accounts, then choose Share on a saved ride.")
                        .font(TypeScale.body).foregroundStyle(Palette.muted)
                    ForEach(uploads.services, id: \.id) { service in
                        UploadServiceSettings(service: service)
                    }
                }.padding(16)
            }
            .background(Palette.paper)
            .navigationTitle("")
            .navigationBarTitleDisplayMode(.inline)
            .tint(Palette.accent)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
        }
    }
}

private struct UploadServiceSettings: View {
    let service: any RideUploadService
    @ObservedObject private var uploads = RideUploads.shared
    @State private var key = ""
    @State private var saved = false
    @State private var message: String?
    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                HStack(spacing: 12) {
                    Image(systemName: "arrow.up.circle")
                        .font(.system(size: 24, weight: .semibold)).foregroundStyle(Palette.accent)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(service.name).font(TypeScale.title).foregroundStyle(Palette.ink)
                        Label(saved ? "Connected" : "Not connected",
                              systemImage: saved ? "checkmark.circle.fill" : "circle")
                            .font(TypeScale.bodyStrong)
                            .foregroundStyle(saved ? Palette.good : Palette.muted)
                    }
                }
                Divider().overlay(Palette.hairline)
                Text(saved ? "Replace API key" : "API key").trackedLabel()
                HStack(spacing: 10) {
                    Image(systemName: "lock").foregroundStyle(Palette.muted)
                    SecureField("Paste your API key", text: $key)
                        .font(TypeScale.body).foregroundStyle(Palette.ink)
                        .textInputAutocapitalization(.never).autocorrectionDisabled()
                        .textFieldStyle(.plain)
                        .accessibilityLabel(saved ? "Replace API key" : "API key")
                }
                .padding(14)
                .background(Palette.paper)
                .clipShape(RoundedRectangle(cornerRadius: 14))
                .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(Palette.hairline, lineWidth: 1))
                Text("Stored securely on this phone.")
                    .font(TypeScale.body).foregroundStyle(Palette.muted)
                PrimaryButton(title: saved ? "Update connection" : "Connect", systemImage: "link",
                              enabled: !key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty) {
                    do {
                        try uploads.save(key, service: service.id)
                        key = ""; saved = true; message = "API key saved securely."
                    } catch { message = error.localizedDescription }
                }
                if saved {
                    Button(role: .destructive) {
                        do {
                            try uploads.save("", service: service.id)
                            key = ""; saved = false; message = "API key removed."
                        } catch { message = error.localizedDescription }
                    } label: { UploadSecondaryLabel(title: "Disconnect", icon: "xmark.circle") }
                    .buttonStyle(.plain)
                }
                Link(destination: service.credentialHelpURL) {
                    HStack(spacing: 8) {
                        Text("Get an API key").font(TypeScale.bodyStrong)
                        Image(systemName: "arrow.up.right").font(.system(size: 13, weight: .semibold))
                    }
                    .foregroundStyle(Palette.accent)
                    .frame(maxWidth: .infinity, minHeight: 44)
                    .contentShape(Rectangle())
                }
                if let message { Text(message).font(TypeScale.body).foregroundStyle(Palette.ink) }
            }
        }
        .task {
            do { saved = !(try UploadKeychain.read(service.id)).isEmpty }
            catch { message = error.localizedDescription }
        }
    }
}

struct RideShareButton: View {
    let file: URL
    @State private var showShare = false

    var body: some View {
        PrimaryButton(title: "Share", systemImage: "paperplane") {
            showShare = true
        }
        .sheet(isPresented: $showShare) { RideShareView(file: file) }
    }
}

private struct RideShareView: View {
    let file: URL
    @ObservedObject private var uploads = RideUploads.shared
    @Environment(\.dismiss) private var dismiss
    @State private var showServices = false
    @State private var connectedIDs = Set<String>()
    @State private var connectionErrors: [String] = []
    @State private var loading = true

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(alignment: .leading, spacing: 16) {
                    Text("Share ride").font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
                    Text("Choose a service to upload this ride to.")
                        .font(TypeScale.body).foregroundStyle(Palette.muted)
                    if loading {
                        ProgressView("Loading connected services…")
                    } else {
                        if connectedIDs.isEmpty {
                            Card {
                                VStack(alignment: .leading, spacing: 8) {
                                    Text("No connected services").font(TypeScale.title).foregroundStyle(Palette.ink)
                                    Text("Connect a service to share your ride.").font(TypeScale.body).foregroundStyle(Palette.muted)
                                }
                            }
                        }
                        ForEach(uploads.services.filter { connectedIDs.contains($0.id) }, id: \.id) { service in
                            let action = uploads.actionID(service.id, file)
                            VStack(alignment: .leading, spacing: 10) {
                                Button { uploads.upload(service, file: file) } label: {
                                    Card {
                                        HStack(spacing: 12) {
                                            Image(systemName: "arrow.up.circle")
                                                .font(.system(size: 24, weight: .semibold)).foregroundStyle(Palette.accent)
                                            VStack(alignment: .leading, spacing: 4) {
                                                Text(service.name).font(TypeScale.title).foregroundStyle(Palette.ink)
                                                Text(uploads.active.contains(action) ? "Uploading ride…" : "Upload this ride")
                                                    .font(TypeScale.body).foregroundStyle(Palette.muted)
                                            }
                                            Spacer()
                                            if uploads.active.contains(action) { ProgressView().tint(Palette.accent) }
                                            else {
                                                Image(systemName: "chevron.right")
                                                    .font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.muted)
                                            }
                                        }
                                    }
                                }
                                .buttonStyle(.plain)
                                .disabled(uploads.active.contains(action))
                                if let message = uploads.messages[action] {
                                    Text(message).font(TypeScale.body).foregroundStyle(Palette.ink)
                                        .accessibilityAddTraits(.updatesFrequently)
                                }
                            }
                        }
                        ForEach(connectionErrors, id: \.self) { message in
                            Text(message).font(TypeScale.body)
                        }
                    }
                    UploadNavigationCard(title: "Manage services", summary: "Connect or update your accounts") {
                        showServices = true
                    }
                }.padding(16)
            }
            .background(Palette.paper)
            .navigationTitle("")
            .tint(Palette.accent)
            .navigationBarTitleDisplayMode(.inline)
            .toolbar { ToolbarItem(placement: .confirmationAction) { Button("Done") { dismiss() } } }
            .sheet(isPresented: $showServices) { UploadServicesView() }
            .task(id: uploads.revision) {
                var connected = Set<String>()
                var errors: [String] = []
                for service in uploads.services {
                    do {
                        if !(try UploadKeychain.read(service.id)).isEmpty { connected.insert(service.id) }
                    } catch { errors.append("\(service.name): \(error.localizedDescription)") }
                }
                connectedIDs = connected
                connectionErrors = errors
                loading = false
            }
        }
    }
}

// Matches the Maps and Sensors navigation cards in Settings.
struct UploadNavigationCard: View {
    let title: String
    let summary: String
    let action: () -> Void

    var body: some View {
        Button(action: action) {
            Card {
                HStack(spacing: 12) {
                    Image(systemName: "link")
                        .font(.system(size: 20, weight: .semibold)).foregroundStyle(Palette.accent)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(title).trackedLabel()
                        Text(summary).font(TypeScale.bodyStrong).foregroundStyle(Palette.ink)
                    }
                    Spacer()
                    Image(systemName: "chevron.right")
                        .font(.system(size: 14, weight: .semibold)).foregroundStyle(Palette.muted)
                }
            }
        }.buttonStyle(.plain)
    }
}

private struct UploadSecondaryLabel: View {
    let title: String
    let icon: String

    var body: some View {
        Label(title, systemImage: icon)
            .font(BarlowFont.condensed(18, .semibold))
            .foregroundStyle(Palette.accent)
            .frame(maxWidth: .infinity)
            .padding(.vertical, 13)
            .background(Palette.surface)
            .clipShape(Capsule())
            .overlay(Capsule().strokeBorder(Palette.accent, lineWidth: 1.5))
    }
}
