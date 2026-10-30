import CoreBluetooth
import Foundation

// The radio under BLEManager's connection state machine: the part of
// CoreBluetooth it drives. In the app it IS CoreBluetooth — CBCentralManager
// and CBPeripheral conform below, with no wrapper objects, so identity checks
// (`peripheral === p`) and every legacy GATT call are unchanged. DEBUG builds
// can run the same state machine on SimRadio (SimRadio.swift), the firmware
// simulator's BLE link over TCP (sim/IOS.md).
//
// Events flow back through RadioEvents: the CoreBluetooth delegate methods in
// BLEManager are thin adapters onto it, and SimRadio calls it directly, so
// connect / fail / disconnect / refusal decisions are made in one place.

/// The central: what BLEManager asks of CBCentralManager.
@MainActor
protocol RadioCentral: AnyObject {
    var state: CBManagerState { get }
    /// A pending connect: no timeout, completes when the peer is in reach.
    func connectPeer(_ p: any RadioPeer)
    /// Cancels an active or pending connect; didDisconnect follows.
    func cancelPeer(_ p: any RadioPeer)
    func retrievePeers(withIdentifiers ids: [UUID]) -> [any RadioPeer]
    func retrieveConnectedPeers(withServices uuids: [CBUUID]) -> [any RadioPeer]
    func scan(for uuids: [CBUUID], allowDuplicates: Bool)
    func stopScan()
}

/// A remote device: CBPeripheral's members that BLEManager uses. The legacy
/// characteristics are CBCharacteristics only CoreBluetooth hands out, so a
/// simulated peer never sees those calls; the link goes through LinkTransport.
@MainActor
protocol RadioPeer: AnyObject {
    var identifier: UUID { get }
    var name: String? { get }
    var state: CBPeripheralState { get }
    var delegate: CBPeripheralDelegate? { get set }
    var canSendWriteWithoutResponse: Bool { get }
    func maximumWriteValueLength(for type: CBCharacteristicWriteType) -> Int
    func discoverServices(_ serviceUUIDs: [CBUUID]?)
    func readValue(for characteristic: CBCharacteristic)
    func writeValue(_ data: Data, for characteristic: CBCharacteristic, type: CBCharacteristicWriteType)
    func setNotifyValue(_ enabled: Bool, for characteristic: CBCharacteristic)
}

extension CBPeripheral: RadioPeer {}

extension CBCentralManager: RadioCentral {
    func connectPeer(_ p: any RadioPeer) {
        guard let cb = p as? CBPeripheral else { return }
        connect(cb, options: nil)
    }
    func cancelPeer(_ p: any RadioPeer) {
        guard let cb = p as? CBPeripheral else { return }
        cancelPeripheralConnection(cb)
    }
    func retrievePeers(withIdentifiers ids: [UUID]) -> [any RadioPeer] {
        retrievePeripherals(withIdentifiers: ids)
    }
    func retrieveConnectedPeers(withServices uuids: [CBUUID]) -> [any RadioPeer] {
        retrieveConnectedPeripherals(withServices: uuids)
    }
    func scan(for uuids: [CBUUID], allowDuplicates: Bool) {
        scanForPeripherals(withServices: uuids, options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: allowDuplicates,
        ])
    }
}

/// What the radio reports, in CoreBluetooth's terms (errors are CBError /
/// CBATTError, as the real stack produces them). BLEManager implements it.
@MainActor
protocol RadioEvents: AnyObject {
    func radioStateChanged(_ state: CBManagerState)
    /// An advertisement; `paired` is the device's "paired to a phone" flag
    /// (nil: firmware that doesn't advertise one).
    func radioDidDiscover(_ p: any RadioPeer, paired: Bool?)
    func radioDidConnect(_ p: any RadioPeer)
    func radioDidFailToConnect(_ p: any RadioPeer, error: Error?)
    func radioDidDisconnect(_ p: any RadioPeer, error: Error?)
    // GATT: the link's part. The legacy characteristics' callbacks stay in
    // BLEManager's CBPeripheralDelegate methods (CoreBluetooth only).
    /// Service discovery finished; `linkOffered`: the link service is there.
    func radioServicesDiscovered(_ p: any RadioPeer, linkOffered: Bool)
    /// The link service's RX and TX were found.
    func radioLinkFound(_ p: any RadioPeer, transport: LinkTransport)
    /// Service Changed: these services' characteristics are gone.
    func radioServicesModified(_ p: any RadioPeer, invalidated: [CBUUID])
    /// TX's notification state (the answer to LinkTransport.subscribe).
    func radioLinkNotifyState(isNotifying: Bool, error: Error?)
    /// A TX notification.
    func radioLinkValue(_ data: Data)
    /// A write to RX with response failed.
    func radioLinkWriteFailed(_ error: Error)
    /// Writes without response can go again.
    func radioReadyToSend()
}
