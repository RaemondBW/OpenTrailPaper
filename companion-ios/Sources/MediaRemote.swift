import AVFoundation
import Foundation
import MediaPlayer
import UIKit

// Feeds the device's MUSIC page and answers its transport buttons.
//
// Built on MPMusicPlayerController.systemMusicPlayer, which is the ONLY
// public way to observe and control playback that another app owns — and it
// only reaches the Apple Music app. Spotify and friends expose nothing to
// third parties on iOS, so with those the page shows "nothing playing"; the
// Android companion (MediaSessionManager) does not share this limit.
//
// Started only while the device's dashboard config contains a music page —
// no page, no Media Library permission prompt and no observers.
@MainActor
final class MediaRemote {
    private weak var ble: BLEManager?
    private let player = MPMusicPlayerController.systemMusicPlayer
    private var observers: [NSObjectProtocol] = []
    private var running = false
    // The artwork last pushed, so a play/pause blip doesn't re-send ~90 KB.
    private var sentArtID: MPMediaEntityPersistentID?

    init(ble: BLEManager) { self.ble = ble }

    func start() {
        guard !running else { return }
        running = true
        // Reading nowPlayingItem needs Media Library access. Ask lazily, here,
        // so only riders who actually add the music page ever see the prompt.
        MPMediaLibrary.requestAuthorization { [weak self] _ in
            Task { @MainActor in self?.beginObserving() }
        }
    }

    private func beginObserving() {
        guard running, observers.isEmpty else { return }
        player.beginGeneratingPlaybackNotifications()
        let nc = NotificationCenter.default
        let names: [Notification.Name] = [
            .MPMusicPlayerControllerNowPlayingItemDidChange,
            .MPMusicPlayerControllerPlaybackStateDidChange,
        ]
        for name in names {
            observers.append(nc.addObserver(forName: name, object: player,
                                            queue: .main) { [weak self] _ in
                Task { @MainActor in self?.pushNow() }
            })
        }
        pushNow()
    }

    func stop() {
        guard running else { return }
        running = false
        for o in observers { NotificationCenter.default.removeObserver(o) }
        observers.removeAll()
        player.endGeneratingPlaybackNotifications()
        sentArtID = nil
        ble?.sendMediaClear()
    }

    /// Transport command from the device (media_state.h MediaCmd).
    func handleCommand(_ cmd: UInt8) {
        switch cmd {
        case 1: player.playbackState == .playing ? player.pause() : player.play()
        case 2: player.skipToNextItem()
        case 3: player.skipToPreviousItem()
        case 4: nudgeVolume(+0.0625)
        case 5: nudgeVolume(-0.0625)
        default: break
        }
        // A state notification follows, but not always promptly — reflect the
        // new state now so the panel settles instead of flip-flopping.
        Task { @MainActor in self.pushNow() }
    }

    // MARK: - System volume

    // The one App-Store-legal way to SET output volume: an MPVolumeView's
    // slider, parked offscreen in the key window. This moves SYSTEM volume, so
    // it works whatever app is playing — including the players iOS won't let
    // us see the metadata of. Known limit: it is UI plumbing, so it may not
    // take while the app is deep in the background; worth knowing on the bike.
    private let volumeView = MPVolumeView(frame: CGRect(x: -2000, y: -2000,
                                                        width: 1, height: 1))

    private func nudgeVolume(_ delta: Float) {
        if volumeView.superview == nil {
            let scene = UIApplication.shared.connectedScenes
                .compactMap { $0 as? UIWindowScene }
                .first { $0.activationState == .foregroundActive || $0.activationState == .foregroundInactive }
            guard let window = scene?.windows.first else { return }
            volumeView.clipsToBounds = true
            window.addSubview(volumeView)
        }
        guard let slider = volumeView.subviews.compactMap({ $0 as? UISlider }).first
        else { return }
        // The slider mirrors the live system volume once it's in a window;
        // AVAudioSession is the fallback the first time, before it syncs.
        let base = slider.value > 0 ? slider.value
                                    : AVAudioSession.sharedInstance().outputVolume
        let v = max(0, min(1, base + delta))
        // A beat later: the slider ignores writes made in the same runloop
        // turn it was attached in.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) {
            slider.value = v
        }
    }

    private func pushNow() {
        guard running, let ble else { return }
        guard let item = player.nowPlayingItem else {
            sentArtID = nil
            ble.sendMediaClear()
            return
        }
        let playing = player.playbackState == .playing
        let pos = UInt16(min(max(player.currentPlaybackTime, 0), 65535))
        let dur = UInt16(min(max(item.playbackDuration, 0), 65535))
        ble.sendMediaMeta(playing: playing, posSec: pos, durSec: dur,
                          title: item.title ?? "",
                          artist: item.artist ?? "",
                          album: item.albumTitle ?? "")
        if sentArtID != item.persistentID {
            sentArtID = item.persistentID
            pushArt(item)
        }
    }

    private func pushArt(_ item: MPMediaItem) {
        // 300 px fits the device's 324 px frame; grayscale 8-bit is what its
        // ditherer wants. No art on the item -> nothing sent; the device draws
        // its vinyl placeholder.
        let side = 300
        guard let art = item.artwork,
              let img = art.image(at: CGSize(width: side, height: side)),
              let gray = Self.grayscaleBytes(img, side: side) else { return }
        ble?.sendMediaArt(gray, width: side, height: side)
    }

    private static func grayscaleBytes(_ img: UIImage, side: Int) -> Data? {
        guard let cg = img.cgImage else { return nil }
        var buf = Data(count: side * side)
        let ok = buf.withUnsafeMutableBytes { raw -> Bool in
            guard let ctx = CGContext(data: raw.baseAddress, width: side,
                                      height: side, bitsPerComponent: 8,
                                      bytesPerRow: side,
                                      space: CGColorSpaceCreateDeviceGray(),
                                      bitmapInfo: CGImageAlphaInfo.none.rawValue)
            else { return false }
            // White ground first: art with alpha must land on paper, not black.
            ctx.setFillColor(gray: 1.0, alpha: 1.0)
            ctx.fill(CGRect(x: 0, y: 0, width: side, height: side))
            ctx.interpolationQuality = .high
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: side, height: side))
            return true
        }
        return ok ? buf : nil
    }
}
