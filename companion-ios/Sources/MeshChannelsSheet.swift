import SwiftUI

// Private channels: create one, show its QR so someone can join, or scan theirs.
//
// A "private channel" here is Meshtastic's own mechanism — a channel with a random
// 256-bit key that only the people who scanned the code hold. Everyone else on the
// mesh sees a packet go by and cannot read a byte of it. A two-person channel is
// therefore a private conversation, and a several-person one is a private group;
// they are the same thing at different sizes.
//
// The QR is the standard meshtastic.org/e/# URL, so the other person does not need
// this app — the official Meshtastic app will import it too.
struct MeshChannelsSheet: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss
    @Binding var selected: UInt8

    @State private var newName = ""
    @State private var creating = false
    @State private var sharing: MeshChannel? = nil
    @State private var scanning = false
    @State private var scanError: String? = nil
    @State private var confirmForget: MeshChannel? = nil
    /// Screenshot hook only: opens the invite sheet for the first private channel.
    var autoShareForDemo = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    ForEach(ble.meshChannels) { c in channelCard(c) }

                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("New private channel").trackedLabel()
                            TextField("Name (e.g. Saturday Ride)", text: $newName)
                                .font(BarlowFont.text(16))
                                .autocorrectionDisabled()
                            Text("Creates a channel with a random 256-bit key. Share its code with the people you want in it — anyone else on the mesh sees the traffic and cannot read it.")
                                .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                            HStack(spacing: 18) {
                                Button("Create") { create() }
                                    .disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty
                                              || ble.firstFreeMeshChannel == nil)
                                Button("Scan someone's code") { scanning = true }
                            }
                            .font(BarlowFont.condensed(18, .semibold))
                            .foregroundStyle(Palette.accent)
                            if ble.firstFreeMeshChannel == nil {
                                Text("All channel slots are in use. Forget one to add another.")
                                    .font(BarlowFont.text(13))
                                    .foregroundStyle(Palette.accent)
                            }
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("How this works").trackedLabel()
                            Text("Every channel shares one radio setting, so a private channel costs no range and no battery. What separates them is the key.")
                                .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                            Text("Both people must also be on the same public channel — that is what sets the frequency you are both listening on. If a code does not work, check you are both on \(ble.meshState.channel).")
                                .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                        }
                    }
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle("Channels")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Done") { dismiss() } }
            }
            .sheet(item: $sharing) { c in MeshShareSheet(channel: c) }
            .onAppear {
                if autoShareForDemo {
                    sharing = ble.meshChannels.first { $0.isPrivate }
                }
            }
            .sheet(isPresented: $scanning) {
                NavigationStack {
                    MeshScannerView { scanned($0) }
                        .ignoresSafeArea()
                        .navigationTitle("Scan a channel")
                        .navigationBarTitleDisplayMode(.inline)
                        .toolbar {
                            ToolbarItem(placement: .topBarTrailing) {
                                Button("Cancel") { scanning = false }
                            }
                        }
                }
            }
            .alert("Could not join", isPresented: Binding(
                get: { scanError != nil }, set: { if !$0 { scanError = nil } })) {
                Button("OK", role: .cancel) { scanError = nil }
            } message: {
                Text(scanError ?? "")
            }
            .alert("Forget this channel?", isPresented: Binding(
                get: { confirmForget != nil },
                set: { if !$0 { confirmForget = nil } })) {
                Button("Forget", role: .destructive) {
                    if let c = confirmForget { ble.forgetMeshChannel(index: c.index) }
                    confirmForget = nil
                }
                Button("Cancel", role: .cancel) { confirmForget = nil }
            } message: {
                Text("The key is deleted along with the messages that arrived on it. You will need the code again to rejoin.")
            }
        }
    }

    private func channelCard(_ c: MeshChannel) -> some View {
        Card {
            VStack(alignment: .leading, spacing: 8) {
                HStack(spacing: 6) {
                    if c.isPrivate {
                        Image(systemName: "lock.fill")
                            .font(.system(size: 12)).foregroundStyle(Palette.good)
                    }
                    Text(c.isPrimary ? "Public · \(c.displayName)" : c.displayName)
                        .font(TypeScale.title).foregroundStyle(Palette.ink)
                    Spacer()
                    if selected == c.index {
                        Image(systemName: "checkmark").foregroundStyle(Palette.accent)
                    }
                }
                Text(c.isPrimary
                     ? "Sets the frequency for every channel · \(c.keyDescription)"
                     : "\(c.keyDescription) · channel byte 0x\(String(format: "%02x", c.hash))")
                    .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                HStack(spacing: 18) {
                    Button("Read this one") { selected = c.index; dismiss() }
                    if c.isPrivate { Button("Share") { sharing = c } }
                    if !c.isPrimary {
                        Button("Forget", role: .destructive) { confirmForget = c }
                    }
                }
                .font(BarlowFont.condensed(17, .semibold))
                .foregroundStyle(Palette.accent)
            }
        }
    }

    private func create() {
        guard let slot = ble.firstFreeMeshChannel else { return }
        let key = MeshChannelURL.randomKey()
        guard key.count == 32 else {
            // randomKey() only returns short on a CSPRNG failure. A predictable
            // key would look private and not be, so refuse rather than proceed.
            scanError = "Could not generate a secure key on this device."
            return
        }
        ble.setMeshPrivateChannel(index: slot,
                                  name: newName.trimmingCharacters(in: .whitespaces),
                                  psk: key)
        newName = ""
        selected = slot
    }

    private func scanned(_ text: String) {
        scanning = false
        let found = MeshChannelURL.parse(text)
        guard let first = found.first else {
            scanError = "That code is not a Meshtastic channel."
            return
        }
        guard !first.psk.isEmpty else {
            scanError = "That channel has no encryption key, so it would not be private."
            return
        }
        guard let slot = ble.firstFreeMeshChannel else {
            scanError = "All channel slots are in use. Forget one first."
            return
        }
        // Imported as a PRIVATE channel, never as the primary: the primary sets
        // the frequency, and taking someone else's would move this device off the
        // mesh it is on. A shared channel works because both ends keep their own
        // primary and add this one alongside it.
        let name = first.name.isEmpty ? "Shared" : first.name
        ble.setMeshPrivateChannel(index: slot, name: name, psk: first.psk)
        selected = slot
    }
}

/// The QR someone else scans to join. Also shows the URL, because pasting a link
/// into a message is often easier than getting two phones in front of each other.
private struct MeshShareSheet: View {
    @Environment(\.dismiss) private var dismiss
    let channel: MeshChannel

    private var shareURL: String {
        MeshChannelURL.url(forChannelNamed: channel.name, psk: channel.psk)
    }

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 16) {
                    if let img = MeshChannelURL.qrImage(for: shareURL) {
                        Image(uiImage: img)
                            .interpolation(.none)          // keep the modules crisp
                            .resizable()
                            .scaledToFit()
                            .frame(maxWidth: 280)
                            .padding(16)
                            .background(Color.white)
                            .clipShape(RoundedRectangle(cornerRadius: 18,
                                                        style: .continuous))
                    }
                    Text(channel.displayName)
                        .font(TypeScale.title).foregroundStyle(Palette.ink)
                    Text("Anyone who scans this joins the channel and can read its messages. It works in the official Meshtastic app too.")
                        .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 24)

                    Card {
                        VStack(alignment: .leading, spacing: 6) {
                            Text("Or send this link").trackedLabel()
                            Text(shareURL)
                                .font(BarlowFont.text(12))
                                .foregroundStyle(Palette.muted)
                                .textSelection(.enabled)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    ShareLink(item: shareURL) {
                        Text("Share link")
                            .font(BarlowFont.condensed(19, .semibold))
                            .frame(maxWidth: .infinity).padding(.vertical, 14)
                            .foregroundStyle(Palette.accentInk)
                            .background(Palette.accent)
                            .clipShape(RoundedRectangle(cornerRadius: 26,
                                                        style: .continuous))
                    }
                    Text("Treat it like a password: anyone who gets the code can read everything on the channel, including messages sent before they joined.")
                        .font(BarlowFont.text(13)).foregroundStyle(Palette.faint)
                        .multilineTextAlignment(.center)
                        .padding(.horizontal, 20)
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle("Invite")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) { Button("Done") { dismiss() } }
            }
        }
    }
}
