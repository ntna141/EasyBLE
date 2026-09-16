#if os(iOS)
import AccessorySetupKit
import CoreBluetooth
import os
import UIKit

private let easyBLELog = Logger(subsystem: "EasyBLE", category: "link")

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
    private var channelOpenHandler: ((EasyBLEIncomingChannel) -> Void)?
    private var channel: EasyBLEIncomingChannel?
    private var sessionReady = false
    private var outgoing = Data()
    private var txPayload: Data?
    private var txType = EasyBLEMessageType.text
    private var txOffset = 0
    private var awaitingResult = false
    private var awaitingOfferAck = false
    private var awaitingChunkAck = false
    private var resultTimeout: Task<Void, Never>?
    private var setupTimeout: Task<Void, Never>?
    private var writeInFlight = false

    public override init() {
        super.init()
        parser.onMessage = { [weak self] type, data in
            self?.received(type: type, data: data)
        }
        parser.onChunk = { [weak self] in
            self?.enqueue(EasyBLEProtocol.ackFrame)
        }
        parser.onResult = { [weak self] status in
            self?.receivedResult(status)
        }
        parser.onAck = { [weak self] in
            self?.receivedAck()
        }
        parser.onChannelHeader = { [weak self] descriptor in
            self?.channelOffered(descriptor)
        }
        parser.onChannelEvent = { [weak self] event in
            self?.receivedChannelEvent(event)
        }
        parser.onError = { [weak self] in
            self?.fail("parser error")
        }
        session.activate(on: .main) { [weak self] event in
            self?.handleSessionEvent(event)
        }
    }

    public func pair(name: String, image: UIImage) async throws {
        easyBLELog.info("pair name=\(name, privacy: .public)")
        let descriptor = ASDiscoveryDescriptor()
        descriptor.bluetoothServiceUUID = CBUUID(string: EasyBLEProtocol.serviceUUID)
        try await session.showPicker(for: [
            ASPickerDisplayItem(name: name, productImage: image, descriptor: descriptor)
        ])
        easyBLELog.info("pair picker finished")
    }

    public func unpair() async throws {
        guard let accessory else {
            easyBLELog.info("unpair skipped no accessory")
            return
        }
        easyBLELog.info("unpair")
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
        let typeName = type == .image ? "image" : "text"
        guard sessionReady else {
            easyBLELog.error("send rejected type=\(typeName, privacy: .public) bytes=\(data.count) reason=not-connected")
            return false
        }
        guard !awaitingResult else {
            easyBLELog.error("send rejected type=\(typeName, privacy: .public) bytes=\(data.count) reason=already-sending")
            return false
        }
        guard !data.isEmpty else {
            easyBLELog.error("send rejected type=\(typeName, privacy: .public) reason=empty")
            return false
        }
        guard data.count <= EasyBLEProtocol.maxMessageSize else {
            easyBLELog.error("send rejected type=\(typeName, privacy: .public) bytes=\(data.count) reason=too-large max=\(EasyBLEProtocol.maxMessageSize)")
            return false
        }
        easyBLELog.info("send start type=\(typeName, privacy: .public) bytes=\(data.count) writeWithoutResponse=\(self.phoneToDevice?.properties.contains(.writeWithoutResponse) == true)")
        awaitingResult = true
        txPayload = data
        txType = type
        txOffset = 0
        awaitingOfferAck = data.count > EasyBLEProtocol.offerThreshold
        armResultTimeout()
        if awaitingOfferAck {
            easyBLELog.info("offer type=\(typeName, privacy: .public) bytes=\(data.count)")
            enqueue(EasyBLEProtocol.offerFrame(type: type, length: data.count))
        } else {
            enqueueNextFrame()
        }
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

    public func onChannelOpen(_ handler: @escaping (EasyBLEIncomingChannel) -> Void) {
        channelOpenHandler = handler
    }

    @discardableResult
    public func requestChannel() -> Bool {
        guard sessionReady else {
            easyBLELog.error("requestChannel rejected reason=not-connected")
            return false
        }
        guard channel == nil else {
            easyBLELog.error("requestChannel rejected reason=channel-open")
            return false
        }
        enqueue(EasyBLEProtocol.controlFrame(true))
        return true
    }

    public func centralManager(_ central: CBCentralManager, willRestoreState dict: [String: Any]) {
        peripheral = (dict[CBCentralManagerRestoredStatePeripheralsKey] as? [CBPeripheral])?.first
        peripheral?.delegate = self
    }

    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        easyBLELog.info("central state=\(central.state.rawValue)")
        guard central.state == .poweredOn else { return }
        reconnect()
    }

    public func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        easyBLELog.info("didConnect \(peripheral.identifier.uuidString, privacy: .public)")
        parser.reset()
        peripheral.delegate = self
        armSetupTimeout()
        peripheral.discoverServices([CBUUID(string: EasyBLEProtocol.serviceUUID)])
    }

    public func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        timestamp: CFAbsoluteTime,
        isReconnecting: Bool,
        error: Error?
    ) {
        easyBLELog.error("didDisconnect reconnecting=\(isReconnecting) error=\(error?.localizedDescription ?? "none", privacy: .public)")
        resetLink()
        if !isReconnecting {
            reconnect()
        }
    }

    public func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error {
            easyBLELog.error("discoverServices \(error.localizedDescription, privacy: .public)")
        }
        guard error == nil,
              let service = peripheral.services?.first(where: {
                  $0.uuid == CBUUID(string: EasyBLEProtocol.serviceUUID)
              }) else {
            easyBLELog.error("EasyBLE service missing")
            return
        }
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
        if let error {
            easyBLELog.error("discoverCharacteristics \(error.localizedDescription, privacy: .public)")
        }
        guard error == nil, let characteristics = service.characteristics else { return }
        let deviceToPhoneUUID = CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID)
        let phoneToDeviceUUID = CBUUID(string: EasyBLEProtocol.phoneToDeviceUUID)
        guard let deviceToPhone = characteristics.first(where: { $0.uuid == deviceToPhoneUUID }),
              let phoneToDevice = characteristics.first(where: { $0.uuid == phoneToDeviceUUID }) else {
            easyBLELog.error("EasyBLE characteristics missing found=\(characteristics.map(\.uuid.uuidString).joined(separator: ","), privacy: .public)")
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
        if let error {
            fail("notify \(error.localizedDescription)")
            return
        }
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID) else { return }
        if characteristic.isNotifying {
            guard !sessionReady else { return }
            easyBLELog.info("session ready mtu=\(peripheral.maximumWriteValueLength(for: .withoutResponse))")
            setupTimeout?.cancel()
            sessionReady = true
            connectHandler?()
        } else if sessionReady {
            fail("notifications stopped")
        }
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            fail("updateValue \(error.localizedDescription)")
            return
        }
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.deviceToPhoneUUID) else { return }
        guard let value = characteristic.value, !value.isEmpty else { return }
        easyBLELog.info("rx \(value.count) bytes")
        parser.append(value)
    }

    public func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        guard characteristic.uuid == CBUUID(string: EasyBLEProtocol.phoneToDeviceUUID) else { return }
        writeInFlight = false
        if let error {
            fail("write \(error.localizedDescription)")
            return
        }
        pumpWrites()
    }

    public func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        pumpWrites()
    }

    private func handleSessionEvent(_ event: ASAccessoryEvent) {
        easyBLELog.info("session event=\(String(describing: event.eventType), privacy: .public)")
        switch event.eventType {
        case .accessoryAdded, .accessoryChanged:
            guard let accessory = event.accessory else { return }
            use(accessory)
        case .activated:
            guard let accessory = session.accessories.first else {
                easyBLELog.info("session activated with no accessory")
                return
            }
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
        easyBLELog.info("use accessory \(accessory.bluetoothIdentifier?.uuidString ?? "nil", privacy: .public)")
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
        guard let central else {
            easyBLELog.info("reconnect skipped no central")
            return
        }
        guard let id = accessory?.bluetoothIdentifier else {
            easyBLELog.info("reconnect skipped no accessory id")
            return
        }
        guard let peripheral = central.retrievePeripherals(withIdentifiers: [id]).first ?? self.peripheral else {
            easyBLELog.error("reconnect no peripheral for \(id.uuidString, privacy: .public)")
            return
        }
        self.peripheral = peripheral
        peripheral.delegate = self
        if peripheral.state == .connected {
            if sessionReady {
                return
            }
            easyBLELog.info("dropping stale link \(id.uuidString, privacy: .public)")
            central.cancelPeripheralConnection(peripheral)
            return
        }
        easyBLELog.info("connect \(id.uuidString, privacy: .public) state=\(peripheral.state.rawValue)")
        central.connect(peripheral, options: [
            CBConnectPeripheralOptionEnableAutoReconnect: true,
        ])
    }

    private func received(type: UInt8, data: Data) {
        guard let type = EasyBLEMessageType(rawValue: type) else {
            fail("unknown message type \(type)")
            return
        }
        let typeName = type == .image ? "image" : "text"
        let preview = String(data: data, encoding: .utf8) ?? "\(data.count) bytes"
        easyBLELog.info("message \(typeName, privacy: .public) \(preview, privacy: .public)")
        enqueue(EasyBLEProtocol.resultFrame(true))
        receiveHandler?(EasyBLEMessage(type: type, data: data))
    }

    private func channelOffered(_ descriptor: Data) {
        channel?.end()
        let opened = EasyBLEIncomingChannel(descriptor: descriptor) { [weak self] channel, enabled in
            self?.sendChannelControl(enabled, for: channel)
        }
        channel = opened
        easyBLELog.info("channel offered descriptor=\(descriptor.count) bytes handler=\(self.channelOpenHandler != nil)")
        channelOpenHandler?(opened)
    }

    private func receivedChannelEvent(_ event: EasyBLEChannelEvent) {
        guard let channel else {
            easyBLELog.error("channel event without offer")
            return
        }
        channel.deliver(event)
        if case .ended = event {
            easyBLELog.info("channel ended by device, acknowledged")
            self.channel = nil
        }
    }

    private func sendChannelControl(_ enabled: Bool, for channel: EasyBLEIncomingChannel) {
        guard channel === self.channel else { return }
        guard sessionReady else {
            easyBLELog.error("channel control dropped enabled=\(enabled) reason=not-connected")
            return
        }
        easyBLELog.info("channel control enabled=\(enabled)")
        enqueue(EasyBLEProtocol.controlFrame(enabled))
        if !enabled {
            self.channel = nil
        }
    }

    private func enqueue(_ frame: Data) {
        outgoing.append(frame)
        pumpWrites()
    }

    private func receivedAck() {
        guard awaitingChunkAck else {
            fail("unexpected ack offset=\(txOffset)")
            return
        }
        awaitingChunkAck = false
        enqueueNextFrame()
    }

    private func receivedResult(_ status: UInt8) {
        easyBLELog.info("result status=\(status) offer=\(self.awaitingOfferAck) awaiting=\(self.awaitingResult) remaining=\(self.txPayload?.count ?? 0)")
        guard awaitingResult, status <= 1 else {
            fail("unexpected result status=\(status) awaiting=\(awaitingResult)")
            return
        }
        if awaitingOfferAck {
            if status != 1 {
                easyBLELog.error("offer rejected")
                finishSend(false)
                return
            }
            awaitingOfferAck = false
            easyBLELog.info("offer accepted")
            enqueueNextFrame()
            return
        }
        if status == 1, txPayload != nil {
            fail("device nack with \(txPayload?.count ?? 0) bytes remaining")
            return
        }
        finishSend(status == 1)
    }

    private func finishSend(_ success: Bool) {
        easyBLELog.info("send finished success=\(success)")
        awaitingResult = false
        awaitingOfferAck = false
        awaitingChunkAck = false
        resultTimeout?.cancel()
        txPayload = nil
        txOffset = 0
        sendResultHandler?(success)
    }

    private func enqueueNextFrame() {
        guard let payload = txPayload else { return }
        let frame = EasyBLEProtocol.frame(type: txType, payload: payload, offset: txOffset)
        let offset = txOffset
        txOffset += min(EasyBLEProtocol.chunkPayloadSize, payload.count - txOffset)
        easyBLELog.info("frame type=\(self.txType.rawValue) offset=\(offset)/\(payload.count) frame=\(frame.count)")
        if txOffset == payload.count {
            txPayload = nil
        } else {
            awaitingChunkAck = true
        }
        armResultTimeout()
        enqueue(frame)
    }

    private func pumpWrites() {
        guard let peripheral, let phoneToDevice else {
            easyBLELog.error("pumpWrites blocked peripheral=\(self.peripheral != nil) characteristic=\(self.phoneToDevice != nil) outgoing=\(self.outgoing.count)")
            return
        }
        let canWriteWithoutResponse = phoneToDevice.properties.contains(.writeWithoutResponse)

        while !outgoing.isEmpty {
            let writeType: CBCharacteristicWriteType
            if canWriteWithoutResponse && peripheral.canSendWriteWithoutResponse {
                writeType = .withoutResponse
            } else if writeInFlight {
                return
            } else {
                writeType = .withResponse
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
            self?.fail("result timeout after \(EasyBLEProtocol.resultTimeout)s")
        }
    }

    private func armSetupTimeout() {
        setupTimeout?.cancel()
        setupTimeout = Task { [weak self] in
            try? await Task.sleep(for: .seconds(EasyBLEProtocol.setupTimeout))
            guard !Task.isCancelled else { return }
            self?.fail("setup timeout after \(EasyBLEProtocol.setupTimeout)s")
        }
    }

    private func fail(_ reason: String) {
        easyBLELog.error("fail \(reason, privacy: .public) connected=\(self.sessionReady) sending=\(self.awaitingResult) offset=\(self.txOffset) outgoing=\(self.outgoing.count)")
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
        awaitingOfferAck = false
        awaitingChunkAck = false
        resultTimeout?.cancel()
        setupTimeout?.cancel()
        parser.reset()
        channel?.end()
        channel = nil
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
