#if os(iOS)
import AccessorySetupKit
import CoreBluetooth
import UIKit

public final class EasyBLE: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private let session = ASAccessorySession()
    private let parser = StreamParser()
    private var accessory: ASAccessory?
    private var central: CBCentralManager?
    private var peripheral: CBPeripheral?
    private var deviceToPhone: CBCharacteristic?
    private var phoneToDevice: CBCharacteristic?
    private var receiveHandler: ((EasyBLEMessage) -> Void)?
    private var connectHandler: (() -> Void)?
    private var disconnectHandler: (() -> Void)?
    private var sendResultHandler: ((Bool) -> Void)?
    private var sessionReady = false
    private var outgoing = Data()
    private var txPayload: Data?
    private var txType = EasyBLEMessageType.text
    private var txOffset = 0
    private var awaitingResult = false
    private var resultTimeout: Task<Void, Never>?
    private var writeInFlight = false

    public override init() {
        super.init()
        parser.onMessage = { [weak self] type, data in
            self?.received(type: type, data: data)
        }
        parser.onResult = { [weak self] status in
            self?.receivedResult(status)
        }
        parser.onError = { [weak self] in
            self?.fail()
        }
        session.activate(on: .main) { [weak self] event in
            self?.handleSessionEvent(event)
        }
    }

    public func pair(name: String, image: UIImage) async throws {
        let descriptor = ASDiscoveryDescriptor()
        descriptor.bluetoothServiceUUID = CBUUID(string: EasyBLEProtocol.serviceUUID)
        try await session.showPicker(for: [
            ASPickerDisplayItem(name: name, productImage: image, descriptor: descriptor)
        ])
    }

    public func unpair() async throws {
        guard let accessory else { return }
        try await session.removeAccessory(accessory)
    }

    @discardableResult
    public func sendText(_ text: String) -> Bool {
        send(.text, data: Data(text.utf8))
    }

    @discardableResult
    public func sendImage(_ data: Data) -> Bool {
        send(.image, data: data)
    }

    @discardableResult
    public func send(_ type: EasyBLEMessageType, data: Data) -> Bool {
        guard sessionReady, !awaitingResult, !data.isEmpty,
              data.count <= EasyBLEProtocol.maxMessageSize else {
            return false
        }
        awaitingResult = true
        txPayload = data
        txType = type
        txOffset = 0
        armResultTimeout()
        pumpWrites()
        return true
    }

    public var isConnected: Bool { sessionReady }

    public var isSending: Bool { awaitingResult }

    public func onReceive(_ handler: @escaping (EasyBLEMessage) -> Void) {
        receiveHandler = handler
    }

    public func onConnect(_ handler: @escaping () -> Void) {
        connectHandler = handler
    }

    public func onDisconnect(_ handler: @escaping () -> Void) {
        disconnectHandler = handler
    }

    public func onSendResult(_ handler: @escaping (Bool) -> Void) {
        sendResultHandler = handler
    }

    public func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        peripheral = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first
        peripheral?.delegate = self
    }

    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else { return }
        reconnect()
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        parser.reset()
        peripheral.delegate = self
        peripheral.discoverServices([CBUUID(string: EasyBLEProtocol.serviceUUID)])
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        timestamp: CFAbsoluteTime,
        isReconnecting: Bool,
        error: Error?
    ) {
        resetLink()
        if !isReconnecting {
            reconnect()
        }
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        guard error == nil,
              let service = peripheral.services?.first(where: {
                  $0.uuid == CBUUID(string: EasyBLEProtocol.serviceUUID)
              }) else { return }
        peripheral.discoverCharacteristics(
            [
                CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID),
                CBUUID(string: EasyBLEProtocol.phoneToDeviceUUID),
            ],
            for: service
        )
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        guard error == nil, let characteristics = service.characteristics else { return }
        let deviceToPhoneUUID = CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID)
        let phoneToDeviceUUID = CBUUID(string: EasyBLEProtocol.phoneToDeviceUUID)
        guard let deviceToPhone = characteristics.first(where: { $0.uuid == deviceToPhoneUUID }),
              let phoneToDevice = characteristics.first(where: { $0.uuid == phoneToDeviceUUID }) else {
            return
        }
        self.deviceToPhone = deviceToPhone
        self.phoneToDevice = phoneToDevice
        peripheral.setNotifyValue(true, for: deviceToPhone)
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if error != nil {
            fail()
            return
        }
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID) else { return }
        if characteristic.isNotifying {
            guard !sessionReady else { return }
            sessionReady = true
            connectHandler?()
        } else if sessionReady {
            fail()
        }
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if error != nil { fail(); return }
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID) else { return }
        guard let value = characteristic.value, !value.isEmpty else { return }
        parser.append(value)
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.phoneToDeviceUUID) else { return }
        writeInFlight = false
        if error != nil {
            fail()
            return
        }
        pumpWrites()
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pumpWrites()
    }

    private func handleSessionEvent(_ event: ASAccessoryEvent) {
        switch event.eventType {
        case .accessoryAdded, .accessoryChanged:
            guard let accessory = event.accessory else { return }
            use(accessory)
        case .activated:
            guard let accessory = session.accessories.first else { return }
            use(accessory)
        case .accessoryRemoved:
            accessory = nil
            central = nil
            peripheral = nil
            resetLink()
        default:
            break
        }
    }

    private func use(_ accessory: ASAccessory) {
        self.accessory = accessory
        if central == nil {
            central = CBCentralManager(
                delegate: self,
                queue: .main,
                options: [CBCentralManagerOptionRestoreIdentifierKey: EasyBLEProtocol.restoreIdentifier]
            )
        } else if central?.state == .poweredOn {
            reconnect()
        }
    }

    private func reconnect() {
        guard let central, let id = accessory?.bluetoothIdentifier else { return }
        guard let peripheral = central.retrievePeripherals(withIdentifiers: [id]).first ?? self.peripheral else {
            return
        }
        self.peripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: [
            CBConnectPeripheralOptionEnableAutoReconnect: true,
        ])
    }

    private func received(type: UInt8, data: Data) {
        guard let type = EasyBLEMessageType(rawValue: type) else {
            fail()
            return
        }
        outgoing.append(EasyBLEProtocol.resultFrame(true))
        pumpWrites()
        receiveHandler?(EasyBLEMessage(type: type, data: data))
    }

    private func receivedResult(_ status: UInt8) {
        guard awaitingResult, status <= 1 else {
            fail()
            return
        }
        if status == 1, txPayload != nil {
            fail()
            return
        }
        finishSend(status == 1)
    }

    private func finishSend(_ success: Bool) {
        awaitingResult = false
        resultTimeout?.cancel()
        txPayload = nil
        txOffset = 0
        sendResultHandler?(success)
    }

    private func nextFrame() -> Data? {
        guard let payload = txPayload else { return nil }
        let frame = EasyBLEProtocol.frame(type: txType, payload: payload, offset: txOffset)
        txOffset += min(EasyBLEProtocol.chunkPayloadSize, payload.count - txOffset)
        if txOffset == payload.count {
            txPayload = nil
        }
        armResultTimeout()
        return frame
    }

    private func pumpWrites() {
        guard let peripheral, let phoneToDevice else { return }
        let writeType: CBCharacteristicWriteType =
            phoneToDevice.properties.contains(.writeWithoutResponse) ? .withoutResponse : .withResponse

        while true {
            if outgoing.isEmpty {
                guard let frame = nextFrame() else { return }
                outgoing = frame
            }
            if writeType == .withoutResponse {
                guard peripheral.canSendWriteWithoutResponse else { return }
            } else if writeInFlight {
                return
            }
            let maxLength = max(1, peripheral.maximumWriteValueLength(for: writeType))
            let chunk = Data(outgoing.prefix(maxLength))
            outgoing.removeFirst(chunk.count)
            peripheral.writeValue(chunk, for: phoneToDevice, type: writeType)
            if writeType == .withResponse {
                writeInFlight = true
                return
            }
        }
    }

    private func armResultTimeout() {
        resultTimeout?.cancel()
        resultTimeout = Task { [weak self] in
            try? await Task.sleep(for: .seconds(EasyBLEProtocol.resultTimeout))
            guard !Task.isCancelled else { return }
            self?.fail()
        }
    }

    private func fail() {
        resetLink()
        if let peripheral {
            central?.cancelPeripheralConnection(peripheral)
        }
    }

    private func resetLink() {
        let wasConnected = sessionReady
        let sendUnresolved = awaitingResult
        sessionReady = false
        awaitingResult = false
        resultTimeout?.cancel()
        parser.reset()
        deviceToPhone = nil
        phoneToDevice = nil
        outgoing.removeAll()
        txPayload = nil
        txOffset = 0
        writeInFlight = false
        if sendUnresolved {
            sendResultHandler?(false)
        }
        if wasConnected {
            disconnectHandler?()
        }
    }
}
#endif
