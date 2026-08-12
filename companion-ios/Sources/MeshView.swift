import SwiftUI

// Mesh text messaging over the head unit's LoRa radio.
//
// The device is a Meshtastic node: it holds the channel key, does the radio work
// and keeps the last few dozen messages. This screen is the keyboard and the
// screen for it — everything shown here was streamed off the device, so a phone
// that has been away still sees what arrived while it was gone.
//
// Deliberately not a per-contact inbox. A mesh channel is one shared
// conversation, the way a radio net is, and direct messages are the exception —
// so the channel is the default view and picking a node narrows it.
struct MeshView: View {
    @EnvironmentObject var ble: BLEManager
    @State private var draft = ""
    /// Who we are talking to, as a node NUMBER rather than a `MeshNode` — the
    /// node list is rebuilt from the device on every refresh, so a stored struct
    /// goes stale (its name and signal are a snapshot) while the number does not.
    @State private var recipientNum: UInt32? = nil    // nil = the whole channel
    @State private var showNodes = false
    @State private var showSettings = false

    /// Whether there is a device to talk to — or seeded demo data standing in
    /// for one.
    private var attached: Bool { ble.state == .connected || ble.demoMesh }

    private var recipient: MeshNode? {
        guard let n = recipientNum else { return nil }
        return ble.meshNodes.first { $0.num == n }
    }

    /// The name to address, live if we know it. Falls back to the id, which is
    /// what a node heard before its NodeInfo arrived has.
    private var recipientName: String {
        guard let n = recipientNum else { return "" }
        return recipient?.displayName ?? "!" + String(format: "%08x", n)
    }

    private var messages: [MeshMessage] {
        guard let n = recipientNum else {
            // The channel view: broadcasts, plus anything we exchanged directly
            // with anyone (a reply to a DM belongs where you can find it).
            return ble.meshMessages
        }
        return ble.meshMessages.filter { $0.from == n || $0.to == n }
    }

    var body: some View {
        NavigationStack {
            VStack(spacing: 0) {
                header
                if !attached {
                    disconnected
                } else if !ble.meshState.enabled {
                    radioOff
                } else if !ble.meshState.radioOk {
                    radioMissing
                } else {
                    thread
                    composer
                }
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationBarHidden(true)
        }
        .sheet(isPresented: $showNodes) {
            MeshNodesSheet(selected: $recipientNum)
        }
        .sheet(isPresented: $showSettings) {
            MeshSettingsSheet()
        }
        .onAppear {
            if ProcessInfo.processInfo.arguments.contains("-demo-mesh") {
                ble.seedDemoMesh()      // screenshots / design review, no device
                return
            }
            ble.refreshMesh()
            ble.markMeshRead()
        }
    }

    // MARK: - Chrome

    private var header: some View {
        HStack(alignment: .firstTextBaseline) {
            VStack(alignment: .leading, spacing: 2) {
                Text("Mesh").font(TypeScale.screenTitle).foregroundStyle(Palette.ink)
                Text(subtitle).trackedLabel()
            }
            Spacer()
            Button { showNodes = true } label: {
                Image(systemName: "person.2")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Palette.ink)
                    .overlay(alignment: .topTrailing) {
                        if ble.meshState.nodeCount > 0 {
                            Text("\(ble.meshState.nodeCount)")
                                .font(BarlowFont.condensed(11, .bold))
                                .foregroundStyle(Palette.accentInk)
                                .padding(.horizontal, 4).padding(.vertical, 1)
                                .background(Palette.accent).clipShape(Capsule())
                                .offset(x: 10, y: -8)
                        }
                    }
            }
            .padding(.trailing, 14)
            Button { showSettings = true } label: {
                Image(systemName: "antenna.radiowaves.left.and.right")
                    .font(.system(size: 17, weight: .semibold))
                    .foregroundStyle(Palette.ink)
            }
        }
        .padding(.horizontal, 16)
        .padding(.top, 8)
        .padding(.bottom, 12)
    }

    private var subtitle: String {
        if !attached { return "not connected" }
        if !ble.meshState.enabled { return "radio off" }
        if recipientNum != nil { return "direct to \(recipientName)" }
        let f = ble.meshState.frequencyMHz
        return String(format: "%@ · %.3f MHz", ble.meshState.channel, f)
    }

    // MARK: - States where there is nothing to show

    private var disconnected: some View {
        emptyState(icon: "antenna.radiowaves.left.and.right.slash",
                   title: "Not connected",
                   body: "Connect to your OpenTrailPaper to read and send mesh messages.")
    }

    private var radioOff: some View {
        VStack(spacing: 16) {
            emptyState(icon: "power",
                       title: "Mesh radio is off",
                       body: "Turn the LoRa radio on to join the mesh. It draws a little power even when idle.")
            PrimaryButton(title: "Turn on the radio") { ble.setMeshEnabled(true) }
                .padding(.horizontal, 32)
        }
        .frame(maxHeight: .infinity)
    }

    private var radioMissing: some View {
        emptyState(icon: "exclamationmark.triangle",
                   title: "No radio found",
                   body: "The device could not talk to its SX1262. On the Lite board there is no LoRa module fitted; otherwise check the device log.")
    }

    private func emptyState(icon: String, title: String, body: String) -> some View {
        VStack(spacing: 10) {
            Image(systemName: icon)
                .font(.system(size: 34, weight: .light))
                .foregroundStyle(Palette.faint)
            Text(title).font(TypeScale.title).foregroundStyle(Palette.ink)
            Text(body)
                .font(BarlowFont.text(15)).foregroundStyle(Palette.muted)
                .multilineTextAlignment(.center)
                .padding(.horizontal, 34)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - The conversation

    private var thread: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(spacing: 10) {
                    if messages.isEmpty {
                        Text(recipientNum == nil
                             ? "Nothing heard yet. Messages from anyone on this channel show up here."
                             : "No messages with this node yet.")
                            .font(BarlowFont.text(15)).foregroundStyle(Palette.muted)
                            .multilineTextAlignment(.center)
                            .padding(.horizontal, 30).padding(.top, 40)
                    }
                    ForEach(messages) { m in
                        MeshBubble(message: m, senderName: name(for: m.from))
                            .id(m.id)
                    }
                }
                .padding(.horizontal, 16)
                .padding(.bottom, 8)
            }
            .onChange(of: messages.count) { _, _ in
                // Follow the conversation, the way any chat does.
                if let last = messages.last {
                    withAnimation { proxy.scrollTo(last.id, anchor: .bottom) }
                }
            }
        }
    }

    private func name(for num: UInt32) -> String {
        if num == ble.meshState.nodeNum { return ble.meshState.longName }
        if let n = ble.meshNodes.first(where: { $0.num == num }) { return n.displayName }
        // Heard before its NodeInfo arrived: the id is the honest answer.
        return "!" + String(format: "%08x", num)
    }

    private var composer: some View {
        VStack(spacing: 6) {
            if ble.meshSendRejected {
                Text("The device could not queue that — its outbox is full. Try again in a moment.")
                    .font(BarlowFont.text(13)).foregroundStyle(Palette.accent)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
            HStack(spacing: 10) {
                if recipientNum != nil {
                    Button { recipientNum = nil } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(Palette.faint)
                    }
                }
                TextField(recipientNum == nil ? "Message the channel"
                                              : "Message \(recipientName)",
                          text: $draft, axis: .vertical)
                    .font(BarlowFont.text(16))
                    .foregroundStyle(Palette.ink)
                    .lineLimit(1...4)
                    .padding(.horizontal, 14).padding(.vertical, 10)
                    .background(Palette.surface)
                    .clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
                    .overlay(RoundedRectangle(cornerRadius: 20, style: .continuous)
                        .strokeBorder(Palette.hairline, lineWidth: 1))

                Button(action: send) {
                    Image(systemName: "arrow.up")
                        .font(.system(size: 17, weight: .bold))
                        .foregroundStyle(Palette.accentInk)
                        .frame(width: 42, height: 42)
                        .background(canSend ? Palette.accent : Palette.faint)
                        .clipShape(Circle())
                }
                .disabled(!canSend)
            }
            // Air time is the real cost on a mesh, so the budget is visible
            // rather than enforced by a silent truncation.
            if bytes > 140 {
                Text("\(bytes)/200 bytes")
                    .font(BarlowFont.condensed(12, .medium))
                    .foregroundStyle(bytes > 200 ? Palette.accent : Palette.faint)
                    .frame(maxWidth: .infinity, alignment: .trailing)
            }
        }
        .padding(.horizontal, 16).padding(.top, 8).padding(.bottom, 10)
        .background(Palette.paper)
        .overlay(alignment: .top) {
            Rectangle().fill(Palette.hairline).frame(height: 1)
        }
    }

    private var bytes: Int { draft.utf8.count }
    private var canSend: Bool {
        !draft.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    private func send() {
        ble.sendMeshText(draft, to: recipientNum)
        draft = ""
    }
}

// One message. Ours on the right, everyone else's on the left — the convention
// every messaging app shares, and the fastest way to read a thread at a glance.
private struct MeshBubble: View {
    let message: MeshMessage
    let senderName: String

    var body: some View {
        HStack {
            if message.outgoing { Spacer(minLength: 40) }
            VStack(alignment: message.outgoing ? .trailing : .leading, spacing: 3) {
                if !message.outgoing {
                    Text(senderName)
                        .font(BarlowFont.condensed(13, .semibold))
                        .foregroundStyle(Palette.accent)
                }
                Text(message.text)
                    .font(BarlowFont.text(16))
                    .foregroundStyle(message.outgoing ? Palette.accentInk : Palette.ink)
                    .multilineTextAlignment(.leading)
                    .fixedSize(horizontal: false, vertical: true)
                HStack(spacing: 6) {
                    Text(message.date, style: .time)
                    if !message.outgoing && message.hops > 0 {
                        Text("· \(message.hops) hop\(message.hops == 1 ? "" : "s")")
                    }
                    if !message.outgoing && message.hops == 0 {
                        Text("· \(message.rssi) dBm")
                    }
                    if message.outgoing { statusMark }
                }
                .font(BarlowFont.condensed(12, .medium))
                .foregroundStyle(message.outgoing ? Palette.accentWash : Palette.faint)
            }
            .padding(.horizontal, 14).padding(.vertical, 10)
            .background(message.outgoing ? Palette.accent : Palette.surface)
            .clipShape(RoundedRectangle(cornerRadius: 18, style: .continuous))
            .overlay(RoundedRectangle(cornerRadius: 18, style: .continuous)
                .strokeBorder(message.outgoing ? .clear : Palette.hairline, lineWidth: 1))
            if !message.outgoing { Spacer(minLength: 40) }
        }
    }

    // Queued / on the air / acknowledged / gave up. A broadcast stops at "sent":
    // there is nobody in particular to acknowledge it, and showing a permanently
    // unacknowledged tick would read as a failure.
    @ViewBuilder private var statusMark: some View {
        switch message.status {
        case .pending: Image(systemName: "clock")
        case .sent:    Image(systemName: "checkmark")
        case .acked:   Image(systemName: "checkmark.circle.fill")
        case .failed:  Image(systemName: "exclamationmark.triangle.fill")
        }
    }
}

// Who else is out there. Tapping a node narrows the thread to a direct
// conversation with it; direct messages are acknowledged, broadcasts are not.
private struct MeshNodesSheet: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss
    @Binding var selected: UInt32?

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 12) {
                    Card {
                        VStack(alignment: .leading, spacing: 4) {
                            Text("This device").trackedLabel()
                            Text(ble.meshState.longName.isEmpty ? ble.meshState.nodeId
                                                                : ble.meshState.longName)
                                .font(TypeScale.title).foregroundStyle(Palette.ink)
                            Text("\(ble.meshState.nodeId) · \(ble.meshState.shortName)")
                                .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                        }
                    }

                    Button {
                        selected = nil
                        dismiss()
                    } label: {
                        Card {
                            HStack {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text("Everyone on \(ble.meshState.channel)")
                                        .font(TypeScale.title).foregroundStyle(Palette.ink)
                                    Text("Broadcast to the whole channel")
                                        .font(BarlowFont.text(13))
                                        .foregroundStyle(Palette.muted)
                                }
                                Spacer()
                                if selected == nil {
                                    Image(systemName: "checkmark")
                                        .foregroundStyle(Palette.accent)
                                }
                            }
                        }
                    }
                    .buttonStyle(.plain)

                    Text("Heard recently").trackedLabel()
                        .frame(maxWidth: .infinity, alignment: .leading)

                    if ble.meshNodes.isEmpty {
                        Text("No neighbours yet. Nodes appear as they transmit — it can take a few minutes on a quiet mesh.")
                            .font(BarlowFont.text(14)).foregroundStyle(Palette.muted)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }

                    ForEach(ble.meshNodes) { n in
                        Button {
                            selected = n.num
                            dismiss()
                        } label: {
                            Card {
                                HStack {
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(n.displayName)
                                            .font(TypeScale.title).foregroundStyle(Palette.ink)
                                        Text(detail(n))
                                            .font(BarlowFont.text(13))
                                            .foregroundStyle(Palette.muted)
                                    }
                                    Spacer()
                                    if selected == n.num {
                                        Image(systemName: "checkmark")
                                            .foregroundStyle(Palette.accent)
                                    }
                                }
                            }
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle("Nodes")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
        }
    }

    private func detail(_ n: MeshNode) -> String {
        var parts = [n.nodeId]
        if n.hops == 0 { parts.append("direct · \(n.rssi) dBm") }
        else { parts.append("\(n.hops) hop\(n.hops == 1 ? "" : "s") away") }
        parts.append(RelativeDateTimeFormatter().localizedString(for: n.lastHeard,
                                                                relativeTo: Date()))
        return parts.joined(separator: " · ")
    }
}

// Radio and identity. Region is absent on purpose: which band the radio may use
// is set at build time in the firmware, because it is a certification question
// rather than a preference.
private struct MeshSettingsSheet: View {
    @EnvironmentObject var ble: BLEManager
    @Environment(\.dismiss) private var dismiss
    @State private var longName = ""
    @State private var shortName = ""
    @State private var channel = ""
    @State private var channelKey: UInt8 = 1
    @State private var confirmChannel = false

    var body: some View {
        NavigationStack {
            ScrollView {
                VStack(spacing: 14) {
                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Radio").trackedLabel()
                            Toggle("LoRa radio", isOn: Binding(
                                get: { ble.meshState.enabled },
                                set: { ble.setMeshEnabled($0) }))
                                .font(BarlowFont.text(16))
                                .tint(Palette.accent)
                            row("Node", ble.meshState.nodeId)
                            row("Frequency",
                                String(format: "%.3f MHz", ble.meshState.frequencyMHz))
                            row("Preset", "LongFast")
                            row("Status", ble.meshState.radioOk ? "up" : "not found")
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("This node's name").trackedLabel()
                            TextField("Long name", text: $longName)
                                .font(BarlowFont.text(16))
                            TextField("Short (4 characters)", text: $shortName)
                                .font(BarlowFont.text(16))
                                .onChange(of: shortName) { _, v in
                                    if v.count > 4 { shortName = String(v.prefix(4)) }
                                }
                            Text("How other people's Meshtastic apps will label your messages.")
                                .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                            Button("Save name") {
                                ble.setMeshNames(long: longName, short: shortName)
                            }
                            .font(BarlowFont.condensed(18, .semibold))
                            .foregroundStyle(Palette.accent)
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 10) {
                            Text("Channel").trackedLabel()
                            TextField("Channel name", text: $channel)
                                .font(BarlowFont.text(16))
                                .autocorrectionDisabled()
                            Stepper("Key \(channelKey)", value: $channelKey, in: 1...10)
                                .font(BarlowFont.text(16))
                            Text("Everyone you want to talk to must use the same name and key. The name also sets the frequency — \"LongFast\" with key 1 is Meshtastic's default channel, which is what a stock node is listening on.")
                                .font(BarlowFont.text(13)).foregroundStyle(Palette.muted)
                            Button("Switch channel") { confirmChannel = true }
                                .font(BarlowFont.condensed(18, .semibold))
                                .foregroundStyle(Palette.accent)
                                .disabled(channel.isEmpty)
                        }
                    }

                    Card {
                        VStack(alignment: .leading, spacing: 8) {
                            Text("Packets").trackedLabel()
                            row("Received", "\(ble.meshStats.rx)")
                            row("Other channels", "\(ble.meshStats.rxOtherChannel)")
                            row("Duplicates", "\(ble.meshStats.rxDuplicate)")
                            row("Dropped", "\(ble.meshStats.rxDropped)")
                            row("Sent", "\(ble.meshStats.tx)")
                            row("Send failures", "\(ble.meshStats.txFailed)")
                            row("Acknowledged", "\(ble.meshStats.acksRx)")
                        }
                    }
                }
                .padding(16)
            }
            .background(Palette.paper.ignoresSafeArea())
            .navigationTitle("Mesh")
            .toolbar {
                ToolbarItem(placement: .topBarTrailing) {
                    Button("Done") { dismiss() }
                }
            }
            .alert("Switch channel?", isPresented: $confirmChannel) {
                Button("Switch", role: .destructive) {
                    ble.setMeshChannel(name: channel, key: channelKey)
                }
                Button("Cancel", role: .cancel) {}
            } message: {
                Text("The device retunes its radio and clears the messages and neighbours it learned on the old channel.")
            }
            .onAppear {
                longName = ble.meshState.longName
                shortName = ble.meshState.shortName
                channel = ble.meshState.channel
                channelKey = ble.meshState.channelKey
                ble.requestMeshStats()
            }
        }
    }

    private func row(_ label: String, _ value: String) -> some View {
        HStack {
            Text(label).font(BarlowFont.text(15)).foregroundStyle(Palette.muted)
            Spacer()
            Text(value).font(BarlowFont.condensed(17, .semibold))
                .foregroundStyle(Palette.ink)
        }
    }
}
