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
                VStack(spacing: 16) {
                    Text("Upload saved rides directly from their ride details. Uploads only happen when you tap Upload.")
                        .font(TypeScale.body).foregroundStyle(Palette.muted)
                    ForEach(uploads.services, id: \.id) { service in
                        UploadServiceSettings(service: service)
                    }
                }.padding(16)
            }
            .background(Palette.paper)
            .navigationTitle("Upload services")
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
                Text(service.name).font(TypeScale.title)
                Text(saved ? "API key saved on this phone" : "No API key saved")
                    .font(TypeScale.body).foregroundStyle(Palette.muted)
                Link("Get an API key in Intervals.icu Settings → Developer Settings",
                     destination: service.credentialHelpURL)
                    .font(TypeScale.body)
                SecureField(saved ? "Replace API key" : "API key", text: $key)
                    .textInputAutocapitalization(.never).autocorrectionDisabled()
                    .textFieldStyle(.roundedBorder)
                Button("Save API key") {
                    do {
                        try uploads.save(key, service: service.id)
                        key = ""; saved = true; message = "API key saved securely."
                    } catch { message = error.localizedDescription }
                }.disabled(key.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                if saved {
                    Button("Disconnect", role: .destructive) {
                        do {
                            try uploads.save("", service: service.id)
                            key = ""; saved = false; message = "API key removed."
                        } catch { message = error.localizedDescription }
                    }
                }
                if let message { Text(message).font(TypeScale.body) }
            }
        }
        .task {
            do { saved = !(try UploadKeychain.read(service.id)).isEmpty }
            catch { message = error.localizedDescription }
        }
    }
}

struct RideUploadButtons: View {
    let file: URL
    @ObservedObject private var uploads = RideUploads.shared
    @State private var showServices = false
    var body: some View {
        Card {
            VStack(alignment: .leading, spacing: 12) {
                Text("Upload ride").trackedLabel()
                ForEach(uploads.services, id: \.id) { service in
                    let action = uploads.actionID(service.id, file)
                    Button {
                        uploads.upload(service, file: file)
                    } label: {
                        HStack {
                            if uploads.active.contains(action) { ProgressView() }
                            Text(uploads.active.contains(action) ? "Uploading…" : "Upload to \(service.name)")
                        }
                    }.disabled(uploads.active.contains(action))
                    if let message = uploads.messages[action] {
                        Text(message).font(TypeScale.body).accessibilityAddTraits(.updatesFrequently)
                    }
                }
                Button("Upload services") { showServices = true }
            }
        }
        .sheet(isPresented: $showServices) { UploadServicesView() }
    }
}
