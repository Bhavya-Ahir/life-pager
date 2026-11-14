import CoreBluetooth
import Foundation
import UIKit

/// Keeps a Bluetooth connection to the pager and relays its frames to the backend.
///
/// The app never interprets the frames (see docs/protocol.md): pager -> notify on TX ->
/// POST /v1/bridge -> write each response frame to RX. iOS relaunches the app in the background
/// for these notifications because of the bluetooth-central background mode plus state restoration.
///
/// Incoming-call alerts do NOT pass through here: iOS delivers them straight to the pager over
/// ANCS once the two are paired, whether or not this app is running.
final class PagerLink: NSObject, ObservableObject {
    enum Status: String {
        case bluetoothOff = "Bluetooth is off"
        case searching = "Looking for pager"
        case connecting = "Connecting"
        case connected = "Connected"
    }

    static let service = CBUUID(string: "7A1E0001-5B0C-4C1E-9F0A-6C1F5E9A2B10")
    static let rxChar = CBUUID(string: "7A1E0002-5B0C-4C1E-9F0A-6C1F5E9A2B10")
    static let txChar = CBUUID(string: "7A1E0003-5B0C-4C1E-9F0A-6C1F5E9A2B10")
    private static let restoreID = "lifepager.central"

    @Published private(set) var status: Status = .searching
    @Published private(set) var lastRelay: Date?
    @Published private(set) var lastError: String?

    private let backend: Backend
    private var central: CBCentralManager!
    private var pager: CBPeripheral?
    private var rx: CBCharacteristic?
    private var pending: [Data] = []
    private var relaying = false

    init(settings: Settings) {
        backend = Backend(settings: settings)
        super.init()
        central = CBCentralManager(delegate: self, queue: .main,
                                   options: [CBCentralManagerOptionRestoreIdentifierKey: Self.restoreID])
    }

    private func connect(_ peripheral: CBPeripheral) {
        pager = peripheral
        peripheral.delegate = self
        status = .connecting
        // No timeout: iOS completes this whenever the pager comes back into range.
        central.connect(peripheral)
    }

    private func search() {
        guard central.state == .poweredOn, pager == nil else { return }
        status = .searching
        if let known = central.retrieveConnectedPeripherals(withServices: [Self.service]).first {
            connect(known)
        } else {
            central.scanForPeripherals(withServices: [Self.service])
        }
    }

    private func relay() {
        guard !relaying, !pending.isEmpty else { return }
        relaying = true
        let frames = Array(pending.prefix(16))
        pending.removeFirst(frames.count)
        let task = UIApplication.shared.beginBackgroundTask(withName: "pager-relay")
        Task { @MainActor in
            defer {
                relaying = false
                UIApplication.shared.endBackgroundTask(task)
                relay()
            }
            do {
                let replies = try await backend.bridge(frames)
                lastRelay = Date()
                lastError = nil
                guard let pager, let rx else { return }
                for frame in replies where frame.count <= pager.maximumWriteValueLength(for: .withResponse) {
                    pager.writeValue(frame, for: rx, type: .withResponse)
                }
            } catch {
                // Dropped on purpose: the pager shows "Sending..." and the user retries. Queueing
                // a "call you back in 10" for later delivery would send it at the wrong moment.
                lastError = error.localizedDescription
            }
        }
    }
}

extension PagerLink: CBCentralManagerDelegate {
    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn {
            if let pager, pager.state != .connected { connect(pager) } else { search() }
        } else {
            status = .bluetoothOff
        }
    }

    func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        if let restored = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first {
            pager = restored
            restored.delegate = self
            if restored.state == .connected {
                status = .connected
                restored.discoverServices([Self.service])
            }
        }
    }

    func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral,
                        advertisementData: [String: Any], rssi RSSI: NSNumber) {
        central.stopScan()
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        status = .connected
        peripheral.discoverServices([Self.service])
    }

    func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        lastError = error?.localizedDescription
        connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        rx = nil
        connect(peripheral)      // pending connection: reconnects when the pager is back in range
    }
}

extension PagerLink: CBPeripheralDelegate {
    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard let service = peripheral.services?.first(where: { $0.uuid == Self.service }) else { return }
        peripheral.discoverCharacteristics([Self.rxChar, Self.txChar], for: service)
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        for characteristic in service.characteristics ?? [] {
            if characteristic.uuid == Self.rxChar { rx = characteristic }
            // Subscribing to this encrypted characteristic is what triggers pairing the first time,
            // and it is the pager's signal that the bridge is up.
            if characteristic.uuid == Self.txChar { peripheral.setNotifyValue(true, for: characteristic) }
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        guard characteristic.uuid == Self.txChar, let frame = characteristic.value, frame.count >= 2 else { return }
        pending.append(frame)
        relay()
    }

    func peripheral(_ peripheral: CBPeripheral, didUpdateNotificationStateFor characteristic: CBCharacteristic, error: Error?) {
        if let error { lastError = "Pairing needed: \(error.localizedDescription)" }
    }
}
