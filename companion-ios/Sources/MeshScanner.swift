import AVFoundation
import AudioToolbox
import SwiftUI
import UIKit

// Camera QR scanner for joining someone's mesh channel.
//
// AVFoundation directly rather than DataScannerViewController: this needs one
// barcode symbology and a single callback, and AVCaptureMetadataOutput has neither
// the device requirements nor the text-recognition machinery attached.
//
// The session is torn down when the view goes away, so the camera is live only
// while the scanner is actually on screen — which is what the usage string in
// Info.plist promises.
struct MeshScannerView: UIViewControllerRepresentable {
    /// Called with the raw scanned string, once. The parent dismisses on the first
    /// good code; the coordinator stops after one either way so a code held in
    /// frame cannot fire repeatedly.
    var onScan: (String) -> Void

    func makeUIViewController(context: Context) -> ScannerController {
        let c = ScannerController()
        c.onScan = onScan
        return c
    }

    func updateUIViewController(_ c: ScannerController, context: Context) {
        c.onScan = onScan
    }

    final class ScannerController: UIViewController,
                                   AVCaptureMetadataOutputObjectsDelegate {
        var onScan: ((String) -> Void)?
        private let session = AVCaptureSession()
        private var preview: AVCaptureVideoPreviewLayer?
        private var delivered = false

        override func viewDidLoad() {
            super.viewDidLoad()
            view.backgroundColor = .black
            configure()
        }

        private func configure() {
            guard let device = AVCaptureDevice.default(for: .video),
                  let input = try? AVCaptureDeviceInput(device: device),
                  session.canAddInput(input) else { return }
            session.addInput(input)

            let output = AVCaptureMetadataOutput()
            guard session.canAddOutput(output) else { return }
            session.addOutput(output)
            output.setMetadataObjectsDelegate(self, queue: .main)
            // Set AFTER adding the output — the available types are empty until
            // then, and asking for .qr too early throws.
            output.metadataObjectTypes = [.qr]

            let layer = AVCaptureVideoPreviewLayer(session: session)
            layer.videoGravity = .resizeAspectFill
            layer.frame = view.bounds
            view.layer.addSublayer(layer)
            preview = layer
        }

        override func viewDidLayoutSubviews() {
            super.viewDidLayoutSubviews()
            preview?.frame = view.bounds
        }

        override func viewWillAppear(_ animated: Bool) {
            super.viewWillAppear(animated)
            guard !session.isRunning else { return }
            // startRunning blocks; off the main thread so presenting the sheet does
            // not hitch.
            DispatchQueue.global(qos: .userInitiated).async { [session] in
                session.startRunning()
            }
        }

        override func viewWillDisappear(_ animated: Bool) {
            super.viewWillDisappear(animated)
            if session.isRunning { session.stopRunning() }
        }

        func metadataOutput(_ output: AVCaptureMetadataOutput,
                            didOutput objects: [AVMetadataObject],
                            from connection: AVCaptureConnection) {
            guard !delivered,
                  let obj = objects.first as? AVMetadataMachineReadableCodeObject,
                  let value = obj.stringValue else { return }
            delivered = true
            AudioServicesPlaySystemSound(1108)   // the system capture blip
            onScan?(value)
        }
    }
}
