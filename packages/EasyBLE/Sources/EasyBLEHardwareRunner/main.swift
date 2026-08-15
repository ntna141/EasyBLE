import CoreBluetooth
import CryptoKit
import EasyBLE
import Foundation

private let serviceUUID = CBUUID(string: EasyBLEProtocol.serviceUUID)
private let streamUUID = CBUUID(string: EasyBLEProtocol.streamUUID)
private let targetName = "EasyBLE-HWTest"

private enum ProtocolValue {
    static let message: UInt8 = 0x01
    static let text = EasyBLEMessageType.text.rawValue
    static let image = EasyBLEMessageType.image.rawValue
}

private enum Fragmentation {
    case normal
    case bytewise
    case cuts([Int])
}

private struct TestAction {
    let name: String
    let run: (HardwareRunner) -> Void
}

private func sha256(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

private func deterministicData(length: Int, seed: UInt8) -> Data {
    Data((0..<length).map { index in
        UInt8(truncatingIfNeeded: Int(seed) + index * 31 + index / 7)
    })
}

private func fnv1a(_ data: Data) -> UInt32 {
    data.reduce(UInt32(2_166_136_261)) { hash, byte in
        (hash ^ UInt32(byte)) &* 16_777_619
    }
}

private func textData(length: Int) -> Data {
    let alphabet = Array("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789\n".utf8)
    return Data((0..<length).map { alphabet[$0 % alphabet.count] })
}

private func frame(type: UInt8, payload: Data) -> Data {
    guard let type = EasyBLEMessageType(rawValue: type) else {
        preconditionFailure("unsupported message type \(type)")
    }
    var output = Data()
    var offset = 0
    while offset < payload.count {
        output.append(EasyBLEProtocol.frame(type: type, payload: payload, offset: offset))
        offset += min(EasyBLEProtocol.chunkPayloadSize, payload.count - offset)
    }
    return output
}

private func resultFrame(_ success: Bool) -> Data {
    EasyBLEProtocol.resultFrame(success)
}

private final class HardwareRunner: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristic: CBCharacteristic?
    private let parser = StreamParser()

    private var actions: [TestAction] = []
    private var actionIndex = 0
    private var passed = 0
    private var startedAt = Date()
    private var connectionCount = 0
    private var actionNotificationBytes = 0
    private var actionNotificationCount = 0
    private var scanTimeout: DispatchWorkItem?
    private var caseTimeout: DispatchWorkItem?

    private var writeQueue: [Data] = []
    private var writeCompletion: (() -> Void)?
    private var writeWithoutResponseQueue: [Data] = []
    private var writeWithoutResponseCompletion: (() -> Void)?

    private var expectedType: UInt8?
    private var expectedMessages: [(data: Data, ack: Bool)] = []
    private var expectedMessageIndex = 0
    private var gotInboundResult = false
    private var expectationCompletion: (() -> Void)?
    private var suppressExpectationPass = false
    private var withholdMessageAck = false
    private var waitingForDisconnectAfterWithhold = false
    private var messageAckDelay: TimeInterval = 0
    private var cancelConnectionOnMessage = false

    private var expectingDisconnect = false
    private var disconnectEarliest: TimeInterval = 0
    private var disconnectLatest: TimeInterval = 0
    private var disconnectClock = Date()
    private var continueAfterDisconnect = true
    private var acceptResultBeforeDisconnect = false

    private var resubscribeAction = false
    private var suiteStarted = false

    override init() {
        super.init()
        parser.onMessage = { [weak self] type, payload in
            self?.receivedMessage(type: type, payload: payload)
        }
        parser.onResult = { [weak self] status in
            self?.receivedResult(status)
        }
        parser.onError = { [weak self] in
            self?.fail("invalid frame from ESP32")
        }
        let environment = ProcessInfo.processInfo.environment
        if let payloadSizeText = environment["EASYBLE_TEST_PAYLOAD_SIZE"],
           let payloadSize = Int(payloadSizeText), payloadSize > 0 {
            let payloadType = environment["EASYBLE_TEST_PAYLOAD_TYPE"] == "image"
                ? ProtocolValue.image : ProtocolValue.text
            actions = [TestAction(name: "boundary-\(payloadType)-\(payloadSize)") { runner in
                runner.sendRoundTrip(
                    type: payloadType,
                    payload: deterministicData(length: payloadSize, seed: 0x6D)
                )
            }]
        } else {
            actions = makeSuite()
            if let filterText = environment["EASYBLE_TEST_FILTER"] {
                actions = actions.filter { $0.name.contains(filterText) }
            }
            if let startText = environment["EASYBLE_TEST_START_INDEX"],
               let oneBasedStart = Int(startText), oneBasedStart > 1 {
                actions = Array(actions.dropFirst(oneBasedStart - 1))
            }
            if let skipText = environment["EASYBLE_TEST_SKIP"] {
                let skipped = skipText.split(separator: ",").map(String.init)
                actions.removeAll { action in skipped.contains { action.name.contains($0) } }
            }
        }
        central = CBCentralManager(delegate: self, queue: .main)
    }

    func centralManagerDidUpdateState(_ central: CBCentralManager) {
        guard central.state == .poweredOn else {
            if central.state != .unknown && central.state != .resetting {
                fail("Bluetooth unavailable: state=\(central.state.rawValue)")
            }
            return
        }
        scan()
    }

    private func scan() {
        guard central.state == .poweredOn, peripheral == nil else { return }
        log(event: "scan", fields: ["service": serviceUUID.uuidString])
        central.scanForPeripherals(withServices: [serviceUUID], options: [
            CBCentralManagerScanOptionAllowDuplicatesKey: false
        ])

        scanTimeout?.cancel()
        let timeout = DispatchWorkItem { [weak self] in
            self?.fail("no EasyBLE-HWTest peripheral found within 12 seconds")
        }
        scanTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + 12, execute: timeout)
    }

    func centralManager(
        _ central: CBCentralManager,
        didDiscover peripheral: CBPeripheral,
        advertisementData: [String: Any],
        rssi RSSI: NSNumber
    ) {
        let advertisedName = advertisementData[CBAdvertisementDataLocalNameKey] as? String
        log(event: "service-candidate", fields: [
            "advertisedName": advertisedName ?? "",
            "peripheralName": peripheral.name ?? "",
            "rssi": RSSI
        ])
        guard advertisedName == targetName || peripheral.name == targetName else { return }
        scanTimeout?.cancel()
        central.stopScan()
        self.peripheral = peripheral
        peripheral.delegate = self
        log(event: "discovered", fields: ["name": advertisedName ?? peripheral.name ?? "", "rssi": RSSI])
        central.connect(peripheral)
    }

    func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        connectionCount += 1
        log(event: "ble-connected", fields: ["connection": connectionCount])
        parser.reset()
        peripheral.discoverServices([serviceUUID])
    }

    func centralManager(
        _ central: CBCentralManager,
        didFailToConnect peripheral: CBPeripheral,
        error: Error?
    ) {
        self.peripheral = nil
        fail("connection failed: \(error?.localizedDescription ?? "unknown")")
    }

    func centralManager(
        _ central: CBCentralManager,
        didDisconnectPeripheral peripheral: CBPeripheral,
        error: Error?
    ) {
        let elapsed = Date().timeIntervalSince(disconnectClock)
        log(event: "ble-disconnected", fields: [
            "elapsed": String(format: "%.3f", elapsed),
            "error": error?.localizedDescription ?? "none"
        ])

        self.peripheral = nil
        characteristic = nil
        parser.reset()
        writeQueue.removeAll()
        writeCompletion = nil
        writeWithoutResponseQueue.removeAll()
        writeWithoutResponseCompletion = nil

        guard expectingDisconnect else {
            fail("unexpected disconnect")
            return
        }

        expectingDisconnect = false
        caseTimeout?.cancel()
        guard elapsed >= disconnectEarliest, elapsed <= disconnectLatest else {
            fail(String(format: "disconnect at %.3fs, expected %.3f...%.3fs",
                        elapsed, disconnectEarliest, disconnectLatest))
            return
        }

        passCurrent(extra: ["disconnectSeconds": String(format: "%.3f", elapsed)], runNext: false)
        if continueAfterDisconnect {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.8) { [weak self] in self?.scan() }
        } else {
            finishSuite()
        }
    }

    func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { fail("service discovery failed: \(error)"); return }
        guard let service = peripheral.services?.first(where: { $0.uuid == serviceUUID }) else {
            fail("EasyBLE service missing")
            return
        }
        peripheral.discoverCharacteristics([streamUUID], for: service)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didDiscoverCharacteristicsFor service: CBService,
        error: Error?
    ) {
        if let error { fail("characteristic discovery failed: \(error)"); return }
        guard let characteristic = service.characteristics?.first(where: { $0.uuid == streamUUID }) else {
            fail("EasyBLE stream characteristic missing")
            return
        }
        self.characteristic = characteristic
        log(event: "characteristic", fields: [
            "properties": characteristic.properties.rawValue,
            "maxWrite": peripheral.maximumWriteValueLength(for: .withResponse)
        ])

        // The ESP32 must not report an EasyBLE connection until this subscription.
        let delay: TimeInterval = suiteStarted ? 0.05 : 1.0
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            peripheral.setNotifyValue(true, for: characteristic)
        }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateNotificationStateFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error { fail("notification state failed: \(error)"); return }

        if resubscribeAction {
            if !characteristic.isNotifying {
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                    peripheral.setNotifyValue(true, for: characteristic)
                }
            } else {
                resubscribeAction = false
                passCurrent()
            }
            return
        }

        guard characteristic.isNotifying else { return }
        if !suiteStarted {
            suiteStarted = true
            log(event: "suite-start", fields: ["cases": actions.count])
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.1) { [weak self] in self?.runCurrentAction() }
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didUpdateValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error { fail("notification failed: \(error)"); return }
        guard let value = characteristic.value else { return }
        actionNotificationBytes += value.count
        actionNotificationCount += 1
        parser.append(value)
    }

    func peripheral(
        _ peripheral: CBPeripheral,
        didWriteValueFor characteristic: CBCharacteristic,
        error: Error?
    ) {
        if let error {
            if expectingDisconnect {
                log(event: "write-ended-during-expected-disconnect", fields: ["error": error.localizedDescription])
                return
            }
            fail("write failed: \(error)")
            return
        }
        guard !writeQueue.isEmpty else { fail("unexpected write completion"); return }
        writeQueue.removeFirst()
        writeNext()
    }

    func peripheralIsReady(toSendWriteWithoutResponse peripheral: CBPeripheral) {
        writeNextWithoutResponse()
    }

    private func enqueueWrite(_ chunks: [Data], completion: (() -> Void)? = nil) {
        guard writeQueue.isEmpty, writeWithoutResponseQueue.isEmpty else {
            fail("test harness attempted overlapping writes")
            return
        }
        writeQueue = chunks.filter { !$0.isEmpty }
        writeCompletion = completion
        writeNext()
    }

    private func enqueueWriteWithoutResponse(_ data: Data, completion: (() -> Void)? = nil) {
        guard let peripheral else { fail("fast write attempted without a peripheral"); return }
        guard writeQueue.isEmpty, writeWithoutResponseQueue.isEmpty else {
            fail("test harness attempted overlapping writes")
            return
        }
        let maximum = max(1, peripheral.maximumWriteValueLength(for: .withoutResponse))
        writeWithoutResponseQueue = stride(from: 0, to: data.count, by: maximum).map {
            data.subdata(in: $0..<min($0 + maximum, data.count))
        }
        writeWithoutResponseCompletion = completion
        writeNextWithoutResponse()
    }

    private func writeNextWithoutResponse() {
        guard let peripheral, let characteristic else {
            fail("fast write attempted without a ready peripheral")
            return
        }
        while peripheral.canSendWriteWithoutResponse,
              let next = writeWithoutResponseQueue.first {
            peripheral.writeValue(next, for: characteristic, type: .withoutResponse)
            writeWithoutResponseQueue.removeFirst()
        }
        if writeWithoutResponseQueue.isEmpty {
            let completion = writeWithoutResponseCompletion
            writeWithoutResponseCompletion = nil
            completion?()
        }
    }

    private func writeNext() {
        guard let peripheral, let characteristic else {
            fail("write attempted without a ready peripheral")
            return
        }
        guard let next = writeQueue.first else {
            let completion = writeCompletion
            writeCompletion = nil
            completion?()
            return
        }
        peripheral.writeValue(next, for: characteristic, type: .withResponse)
    }

    private func chunks(for data: Data, fragmentation: Fragmentation) -> [Data] {
        guard let peripheral else { return [] }
        let maximum = max(1, peripheral.maximumWriteValueLength(for: .withResponse))

        switch fragmentation {
        case .normal:
            return stride(from: 0, to: data.count, by: maximum).map {
                data.subdata(in: $0..<min($0 + maximum, data.count))
            }
        case .bytewise:
            return data.map { Data([$0]) }
        case .cuts(let requestedCuts):
            let cuts = ([0] + requestedCuts + [data.count])
                .map { min(max(0, $0), data.count) }
                .sorted()
            return zip(cuts, cuts.dropFirst()).compactMap { start, end in
                start == end ? nil : data.subdata(in: start..<end)
            }
        }
    }

    private func startExpectation(
        type: UInt8,
        messages: [(Data, Bool)],
        timeout: TimeInterval = 8,
        completion: (() -> Void)? = nil,
        suppressPass: Bool = false
    ) {
        expectedType = type
        expectedMessages = messages.map { (data: $0.0, ack: $0.1) }
        expectedMessageIndex = 0
        gotInboundResult = false
        expectationCompletion = completion
        suppressExpectationPass = suppressPass
        armCaseTimeout(timeout)
    }

    private func sendRoundTrip(
        type: UInt8,
        payload: Data,
        fragmentation: Fragmentation = .normal,
        completion: (() -> Void)? = nil,
        suppressPass: Bool = false
    ) {
        startExpectation(type: type, messages: [(payload, true)],
                         timeout: payload.count >= 4095 ? 14 : 8,
                         completion: completion,
                         suppressPass: suppressPass)
        let bytes = frame(type: type, payload: payload)
        enqueueWrite(chunks(for: bytes, fragmentation: fragmentation))
    }

    private func sendCommand(
        _ command: String,
        expected: [(String, Bool)],
        timeout: TimeInterval = 8
    ) {
        let payload = Data(command.utf8)
        startExpectation(type: ProtocolValue.text,
                         messages: expected.map { (Data($0.0.utf8), $0.1) },
                         timeout: timeout)
        enqueueWrite(chunks(for: frame(type: ProtocolValue.text, payload: payload), fragmentation: .normal))
    }

    private func receivedResult(_ status: UInt8) {
        if expectingDisconnect && acceptResultBeforeDisconnect && expectedType == nil {
            guard status == 1 else { fail("ESP32 returned RESULT status \(status) before disconnect"); return }
            acceptResultBeforeDisconnect = false
            log(event: "accepted-final-result-before-disconnect")
            return
        }
        guard status == 1 else { fail("ESP32 returned RESULT status \(status)"); return }
        guard expectedType != nil, !gotInboundResult else {
            fail("unsolicited or duplicate RESULT from ESP32")
            return
        }
        gotInboundResult = true
        completeExpectationIfReady()
    }

    private func receivedMessage(type: UInt8, payload: Data) {
        guard let expectedType else {
            fail("unexpected peer message type=\(type), length=\(payload.count)")
            return
        }
        guard type == expectedType else {
            fail(String(format: "message type 0x%02x, expected 0x%02x", type, expectedType))
            return
        }
        guard expectedMessageIndex < expectedMessages.count else {
            fail("duplicate peer message")
            return
        }

        let expectation = expectedMessages[expectedMessageIndex]
        guard payload == expectation.data else {
            fail("payload mismatch: got \(payload.count)/\(sha256(payload)), expected \(expectation.data.count)/\(sha256(expectation.data))")
            return
        }
        expectedMessageIndex += 1

        if cancelConnectionOnMessage {
            cancelConnectionOnMessage = false
            expectDisconnect(earliest: 0, latest: 5)
            if let peripheral { central.cancelPeripheralConnection(peripheral) }
            return
        }

        if withholdMessageAck {
            withholdMessageAck = false
            waitingForDisconnectAfterWithhold = true
            disconnectClock = Date()
            armCaseTimeout(disconnectLatest + 3)
            log(event: "withholding-result", fields: ["payloadBytes": payload.count])
            return
        }

        let acknowledge = { [weak self] in
            guard let self else { return }
            self.enqueueWrite(self.chunks(for: resultFrame(expectation.ack), fragmentation: .normal)) { [weak self] in
                self?.completeExpectationIfReady()
            }
        }
        if messageAckDelay > 0 {
            let delay = messageAckDelay
            messageAckDelay = 0
            DispatchQueue.main.asyncAfter(deadline: .now() + delay, execute: acknowledge)
        } else {
            acknowledge()
        }
    }

    private func completeExpectationIfReady() {
        guard !waitingForDisconnectAfterWithhold else { return }
        guard gotInboundResult, expectedMessageIndex == expectedMessages.count, writeQueue.isEmpty else { return }
        caseTimeout?.cancel()
        expectedType = nil
        expectedMessages = []
        let completion = expectationCompletion
        expectationCompletion = nil
        if let completion {
            completion()
        } else if !suppressExpectationPass {
            passCurrent()
        }
    }

    private func runCurrentAction() {
        guard characteristic != nil else { return }
        guard actionIndex < actions.count else { finishSuite(); return }
        resetActionState()
        log(event: "case-start", fields: ["index": actionIndex + 1, "name": actions[actionIndex].name])
        actions[actionIndex].run(self)
    }

    private func resetActionState() {
        caseTimeout?.cancel()
        expectedType = nil
        expectedMessages = []
        expectedMessageIndex = 0
        gotInboundResult = false
        expectationCompletion = nil
        suppressExpectationPass = false
        withholdMessageAck = false
        waitingForDisconnectAfterWithhold = false
        messageAckDelay = 0
        cancelConnectionOnMessage = false
        expectingDisconnect = false
        continueAfterDisconnect = true
        acceptResultBeforeDisconnect = false
        actionNotificationBytes = 0
        actionNotificationCount = 0
    }

    private func passCurrent(extra: [String: Any] = [:], runNext: Bool = true) {
        guard actionIndex < actions.count else { return }
        caseTimeout?.cancel()
        var fields = extra
        fields["index"] = actionIndex + 1
        fields["name"] = actions[actionIndex].name
        log(event: "pass", fields: fields)
        passed += 1
        actionIndex += 1
        if runNext {
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.03) { [weak self] in self?.runCurrentAction() }
        }
    }

    private func fail(_ message: String) {
        caseTimeout?.cancel()
        scanTimeout?.cancel()
        let name = actionIndex < actions.count ? actions[actionIndex].name : "setup"
        log(event: "fail", fields: [
            "index": actionIndex + 1,
            "name": name,
            "reason": message,
            "notificationBytes": actionNotificationBytes,
            "notifications": actionNotificationCount
        ])
        fflush(stdout)
        Darwin.exit(1)
    }

    private func armCaseTimeout(_ seconds: TimeInterval) {
        caseTimeout?.cancel()
        let timeout = DispatchWorkItem { [weak self] in self?.fail("case timed out after \(seconds) seconds") }
        caseTimeout = timeout
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds, execute: timeout)
    }

    private func expectDisconnect(
        earliest: TimeInterval,
        latest: TimeInterval,
        continueAfter: Bool = true
    ) {
        expectingDisconnect = true
        disconnectEarliest = earliest
        disconnectLatest = latest
        disconnectClock = Date()
        continueAfterDisconnect = continueAfter
        armCaseTimeout(latest + 3)
    }

    private func expectProtocolDisconnect(bytes: Data) {
        expectDisconnect(earliest: 0, latest: 5)
        enqueueWrite(chunks(for: bytes, fragmentation: .normal))
    }

    private func partialResume(type: UInt8, payload: Data, cut: Int) {
        let bytes = frame(type: type, payload: payload)
        startExpectation(type: type, messages: [(payload, true)], timeout: 28)
        enqueueWrite(chunks(for: bytes.subdata(in: 0..<cut), fragmentation: .normal)) { [weak self] in
            DispatchQueue.main.asyncAfter(deadline: .now() + 20) {
                guard let self else { return }
                self.enqueueWrite(self.chunks(for: bytes.subdata(in: cut..<bytes.count), fragmentation: .normal))
            }
        }
    }

    private func stress(count: Int, size: Int, maximum: Bool) {
        var iteration = 0
        let started = Date()
        func runOne() {
            if iteration == count {
                let elapsed = Date().timeIntervalSince(started)
                self.passCurrent(extra: [
                    "messages": count,
                    "bytesEach": size,
                    "seconds": String(format: "%.3f", elapsed),
                    "bytesPerSecond": Int(Double(count * size) / max(elapsed, 0.001))
                ])
                return
            }
            let type = iteration.isMultiple(of: 2) ? ProtocolValue.text : ProtocolValue.image
            let data = maximum ? deterministicData(length: size, seed: UInt8(truncatingIfNeeded: iteration))
                               : textData(length: size)
            iteration += 1
            self.sendRoundTrip(type: type, payload: data, completion: runOne, suppressPass: true)
        }
        runOne()
    }

    private func restartAction(command: String) {
        expectDisconnect(earliest: 0, latest: 5)
        acceptResultBeforeDisconnect = true
        let payload = Data(command.utf8)
        enqueueWrite(chunks(for: frame(type: ProtocolValue.text, payload: payload), fragmentation: .normal))
    }

    private func timeoutAction() {
        let command = Data("@cmd:timeout-probe".utf8)
        startExpectation(type: ProtocolValue.text,
                         messages: [(Data("@report:timeout-probe".utf8), true)],
                         timeout: 22)
        // Override message handling semantics by expecting the payload but withholding its ACK.
        expectedMessages = [(Data("@report:timeout-probe".utf8), ack: true)]
        enqueueWrite(chunks(for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
    }

    private func makeSuite() -> [TestAction] {
        var suite: [TestAction] = []

        func roundTrip(_ name: String, _ type: UInt8, _ data: Data, _ fragmentation: Fragmentation = .normal) {
            suite.append(TestAction(name: name) { runner in
                runner.sendRoundTrip(type: type, payload: data, fragmentation: fragmentation)
            })
        }

        func generatedSend(_ name: String, type: UInt8, length: Int,
                           seed: UInt8, timeout: TimeInterval) {
            suite.append(TestAction(name: name) { runner in
                let expected = deterministicData(length: length, seed: seed)
                let started = Date()
                runner.startExpectation(
                    type: type,
                    messages: [(expected, true)],
                    timeout: timeout,
                    completion: {
                        let elapsed = Date().timeIntervalSince(started)
                        runner.passCurrent(extra: [
                            "bytes": length,
                            "seconds": String(format: "%.3f", elapsed),
                            "bytesPerSecond": Int(Double(length) / max(elapsed, 0.001)),
                            "sha256": sha256(expected)
                        ])
                    },
                    suppressPass: true
                )
                let command = "@cmd:send-generated:\(length):\(type):\(seed)"
                runner.enqueueWrite(runner.chunks(
                    for: frame(type: ProtocolValue.text, payload: Data(command.utf8)),
                    fragmentation: .normal))
            })
        }

        for length in [1, 7, 19, 20, 21, 237, 238, 239, 243, 244, 245, 255, 256,
                       511, 512, 1023, 1024, 2048, 4095, 4096] {
            roundTrip("text-\(length)", ProtocolValue.text, textData(length: length))
        }
        roundTrip("text-unicode", ProtocolValue.text, Data("Hello, café 👋 世界 — مرحبا".utf8))
        roundTrip("text-multiline", ProtocolValue.text, Data("first\nsecond\r\nthird\n".utf8))
        roundTrip("text-embedded-nul", ProtocolValue.text, Data([0x41, 0x00, 0x42, 0x00, 0x43]))

        let onePixelPNG = Data(base64Encoded:
            "iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mNk+A8AAQUBAScY42YAAAAASUVORK5CYII=")!
        roundTrip("image-small-valid-png", ProtocolValue.image, onePixelPNG)
        for length in [255, 512, 2048, 4095, 4096] {
            roundTrip("image-binary-\(length)", ProtocolValue.image,
                      deterministicData(length: length, seed: 0xA5))
        }
        roundTrip("image-zero-ff", ProtocolValue.image,
                  Data((0..<2048).map { $0.isMultiple(of: 2) ? 0x00 : 0xFF }))

        generatedSend("chunked-send-3071", type: ProtocolValue.text,
                      length: 3_071, seed: 0x11, timeout: 30)
        generatedSend("chunked-send-3072", type: ProtocolValue.image,
                      length: 3_072, seed: 0x12, timeout: 30)
        generatedSend("chunked-send-3073", type: ProtocolValue.text,
                      length: 3_073, seed: 0x13, timeout: 30)
        generatedSend("retry-send-3200", type: ProtocolValue.image,
                      length: 3_200, seed: 0x16, timeout: 30)
        generatedSend("retry-send-4095", type: ProtocolValue.text,
                      length: 4_095, seed: 0x17, timeout: 30)
        generatedSend("chunked-send-6144", type: ProtocolValue.image,
                      length: 6_144, seed: 0x14, timeout: 30)
        generatedSend("chunked-send-6145", type: ProtocolValue.text,
                      length: 6_145, seed: 0x15, timeout: 30)
        generatedSend("chunked-send-65536", type: ProtocolValue.image,
                      length: 65_536, seed: 0x21, timeout: 60)
        generatedSend("chunked-send-200000", type: ProtocolValue.image,
                      length: 200_000, seed: 0x22, timeout: 120)
        generatedSend("chunked-send-320x240-rgb565", type: ProtocolValue.image,
                      length: 320 * 240 * 2, seed: 0x31, timeout: 120)
        generatedSend("chunked-send-480x320-rgb565", type: ProtocolValue.image,
                      length: 480 * 320 * 2, seed: 0x32, timeout: 180)
        generatedSend("chunked-send-800x480-rgb565", type: ProtocolValue.image,
                      length: 800 * 480 * 2, seed: 0x33, timeout: 300)
        generatedSend("chunked-send-1024x600-rgb565", type: ProtocolValue.image,
                      length: 1024 * 600 * 2, seed: 0x34, timeout: 420)
        generatedSend("chunked-send-1500000", type: ProtocolValue.text,
                      length: 1_500_000, seed: 0x41, timeout: 480)
        generatedSend("chunked-send-2000000", type: ProtocolValue.image,
                      length: 2_000_000, seed: 0x42, timeout: 600)
        generatedSend("chunked-send-3000000", type: ProtocolValue.image,
                      length: 3_000_000, seed: 0x43, timeout: 900)

        func sinkReceive(_ name: String, type: UInt8, length: Int,
                         timeout: TimeInterval, fast: Bool) {
            var payload = deterministicData(length: length, seed: 0x91)
            payload.replaceSubrange(0..<6, with: Data("@sink:".utf8))
            let report = String(
                format: "@report:sink:type=%u,length=%d,fnv=%08x,nul=1",
                type, length, fnv1a(payload)
            )
            suite.append(TestAction(name: name) { runner in
                runner.startExpectation(
                    type: ProtocolValue.text,
                    messages: [(Data(report.utf8), true)],
                    timeout: timeout
                )
                let bytes = frame(type: type, payload: payload)
                if fast {
                    runner.enqueueWriteWithoutResponse(bytes)
                } else {
                    runner.enqueueWrite(runner.chunks(for: bytes, fragmentation: .normal))
                }
            })
        }

        sinkReceive("incoming-only-4096-integrity", type: ProtocolValue.text,
                    length: 4096, timeout: 8, fast: false)

        suite.append(TestAction(name: "large-screen-restart-1500000") {
            $0.restartAction(command: "@cmd:restart-large")
        })
        sinkReceive("large-screen-320x240-rgb565", type: ProtocolValue.image,
                    length: 320 * 240 * 2, timeout: 90, fast: true)
        sinkReceive("large-screen-480x320-rgb565", type: ProtocolValue.image,
                    length: 480 * 320 * 2, timeout: 120, fast: true)
        sinkReceive("large-screen-800x480-rgb565", type: ProtocolValue.image,
                    length: 800 * 480 * 2, timeout: 180, fast: true)
        sinkReceive("large-screen-1024x600-rgb565", type: ProtocolValue.image,
                    length: 1024 * 600 * 2, timeout: 240, fast: true)
        sinkReceive("large-screen-exact-1500000", type: ProtocolValue.image,
                    length: 1_500_000, timeout: 300, fast: true)
        suite.append(TestAction(name: "large-screen-reject-1500001") { runner in
            var bytes = Data([ProtocolValue.message, ProtocolValue.image])
            var declaredLength = UInt32(1_500_001).littleEndian
            withUnsafeBytes(of: &declaredLength) { bytes.append(contentsOf: $0) }
            runner.expectProtocolDisconnect(bytes: bytes)
        })
        suite.append(TestAction(name: "large-screen-restore-default") {
            $0.restartAction(command: "@cmd:restart-default")
        })

        suite.append(TestAction(name: "command-status") { $0.sendCommand(
            "@cmd:status", expected: [("@report:status:connected=1,sending=0", true)])
        })
        suite.append(TestAction(name: "sendText-null-rejected") { $0.sendCommand(
            "@cmd:send-null", expected: [("@report:send-null:false", true)])
        })
        suite.append(TestAction(name: "sendText-empty-rejected") { $0.sendCommand(
            "@cmd:send-empty", expected: [("@report:send-empty:false", true)])
        })
        suite.append(TestAction(name: "send-zero-rejected") { $0.sendCommand(
            "@cmd:send-zero", expected: [("@report:send-zero:false", true)])
        })
        suite.append(TestAction(name: "send-raw-null-rejected") { $0.sendCommand(
            "@cmd:send-raw-null", expected: [("@report:send-raw-null:false", true)])
        })
        suite.append(TestAction(name: "sendText-stops-at-nul") { $0.sendCommand(
            "@cmd:sendtext-nul", expected: [("A", true)])
        })
        suite.append(TestAction(name: "send-over-4096-rejected") { $0.sendCommand(
            "@cmd:send-over", expected: [("@report:send-over:false", true)])
        })
        suite.append(TestAction(name: "second-send-while-awaiting-result") { $0.sendCommand(
            "@cmd:send-double", expected: [("@report:send-double:first", true)])
        })
        suite.append(TestAction(name: "second-begin-rejected") { $0.sendCommand(
            "@cmd:begin-twice", expected: [("@report:begin-twice:false", true)])
        })
        suite.append(TestAction(name: "negative-result-callback") { $0.sendCommand(
            "@cmd:result-failure",
            expected: [("@report:result-failure:probe", false), ("@report:send-result:false", true)])
        })
        suite.append(TestAction(name: "outbound-invalid-type-currently-accepted") { runner in
            let command = Data("@cmd:send-invalid-type".utf8)
            runner.startExpectation(type: 0x7F, messages: [(Data([0xA5]), true)])
            runner.enqueueWrite(runner.chunks(
                for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
        })

        roundTrip("fragment-bytewise", ProtocolValue.text, textData(length: 64), .bytewise)
        roundTrip("fragment-every-header-field", ProtocolValue.text, textData(length: 512),
                  .cuts([1, 2, 3, 4, 5, 6, 7, 63, 244, 245]))
        roundTrip("ack-and-next-parser-baseline", ProtocolValue.image,
                  deterministicData(length: 4096, seed: 0x33))

        suite.append(TestAction(name: "partial-header-pause-20s") { runner in
            runner.partialResume(type: ProtocolValue.text, payload: textData(length: 128), cut: 3)
        })
        suite.append(TestAction(name: "partial-payload-pause-20s") { runner in
            runner.partialResume(type: ProtocolValue.image,
                                 payload: deterministicData(length: 1024, seed: 0x77), cut: 333)
        })

        suite.append(TestAction(name: "unsubscribe-resubscribe") { runner in
            guard let peripheral = runner.peripheral, let characteristic = runner.characteristic else {
                runner.fail("missing subscription")
                return
            }
            runner.resubscribeAction = true
            runner.armCaseTimeout(5)
            peripheral.setNotifyValue(false, for: characteristic)
        })
        roundTrip("post-resubscribe-clean-frame", ProtocolValue.text, Data("clean".utf8))

        suite.append(TestAction(name: "restart-with-minimum-256") { $0.restartAction(command: "@cmd:restart-256") })
        roundTrip("minimum-config-256-byte-message", ProtocolValue.image,
                  deterministicData(length: 256, seed: 0x19))
        suite.append(TestAction(name: "minimum-config-reject-257") { runner in
            runner.expectProtocolDisconnect(bytes: frame(
                type: ProtocolValue.image,
                payload: deterministicData(length: 257, seed: 0x20)
            ))
        })
        suite.append(TestAction(name: "restore-default-4096") { $0.restartAction(command: "@cmd:restart-default") })
        roundTrip("post-restart-maximum-frame", ProtocolValue.image,
                  deterministicData(length: 4096, seed: 0x44))

        suite.append(TestAction(name: "hard-reset-during-connection") {
            $0.restartAction(command: "@cmd:hard-reset")
        })
        roundTrip("hard-reset-recovery-probe", ProtocolValue.text, Data("hard-reset-recovered".utf8))

        let malformed: [(String, Data)] = [
            ("malformed-unknown-opcode", Data([0x7F])),
            ("malformed-unknown-type", Data([0x01, 0x7F])),
            ("malformed-zero-length", Data([0x01, 0x01, 0, 0, 0, 0])),
            ("malformed-over-limit", Data([0x01, 0x02, 0x01, 0x10, 0, 0])),
            ("malformed-result-status", Data([0x02, 0x7F])),
            ("malformed-unsolicited-result", Data([0x02, 0x01]))
        ]
        for (name, bytes) in malformed {
            suite.append(TestAction(name: name) { $0.expectProtocolDisconnect(bytes: bytes) })
            roundTrip("recovery-after-\(name)", ProtocolValue.text, Data("recovered".utf8))
        }

        suite.append(TestAction(name: "duplicate-result-disconnects") { runner in
            runner.sendRoundTrip(
                type: ProtocolValue.text,
                payload: Data("duplicate-result-probe".utf8),
                completion: {
                    runner.expectDisconnect(earliest: 0, latest: 5)
                    runner.enqueueWrite(runner.chunks(for: resultFrame(true), fragmentation: .normal))
                },
                suppressPass: true
            )
        })
        roundTrip("recovery-after-duplicate-result", ProtocolValue.text, Data("duplicate-recovered".utf8))

        suite.append(TestAction(name: "disconnect-with-send-unresolved") { runner in
            let command = Data("@cmd:timeout-probe".utf8)
            runner.startExpectation(
                type: ProtocolValue.text,
                messages: [(Data("@report:timeout-probe".utf8), true)]
            )
            runner.cancelConnectionOnMessage = true
            runner.enqueueWrite(runner.chunks(
                for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
        })
        roundTrip("recovery-after-unresolved-send-disconnect", ProtocolValue.text,
                  Data("unresolved-recovered".utf8))

        suite.append(TestAction(name: "rx-ring-overflow-disconnects") { runner in
            let command = Data("@cmd:pause-update".utf8)
            runner.startExpectation(
                type: ProtocolValue.text,
                messages: [(Data("@report:pause-update".utf8), true)],
                completion: {
                    runner.expectDisconnect(earliest: 1.0, latest: 5.0)
                    let maximum = frame(
                        type: ProtocolValue.image,
                        payload: deterministicData(length: 4096, seed: 0xE1)
                    )
                    runner.enqueueWrite(runner.chunks(
                        for: maximum + maximum, fragmentation: .normal))
                },
                suppressPass: true
            )
            runner.enqueueWrite(runner.chunks(
                for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
        })
        roundTrip("recovery-after-rx-overflow", ProtocolValue.text, Data("overflow-recovered".utf8))

        suite.append(TestAction(name: "partial-frame-central-disconnect") { runner in
            let partial = frame(type: ProtocolValue.image,
                                payload: deterministicData(length: 1024, seed: 0x55)).prefix(200)
            runner.expectDisconnect(earliest: 0, latest: 5)
            runner.enqueueWrite(runner.chunks(for: Data(partial), fragmentation: .normal)) {
                if let peripheral = runner.peripheral { runner.central.cancelPeripheralConnection(peripheral) }
            }
        })
        roundTrip("parser-reset-after-partial-disconnect", ProtocolValue.text, Data("reset-ok".utf8))

        for cycle in 1...20 {
            suite.append(TestAction(name: "rapid-disconnect-\(cycle)") { runner in
                runner.expectDisconnect(earliest: 0, latest: 5)
                if let peripheral = runner.peripheral { runner.central.cancelPeripheralConnection(peripheral) }
            })
            roundTrip("rapid-reconnect-\(cycle)-probe", ProtocolValue.text, Data("cycle-\(cycle)".utf8))
        }

        suite.append(TestAction(name: "stress-1000-short-alternating") {
            $0.stress(count: 1000, size: 32, maximum: false)
        })
        suite.append(TestAction(name: "stress-100-maximum-alternating") {
            $0.stress(count: 100, size: 4096, maximum: true)
        })

        suite.append(TestAction(name: "result-at-14s-beats-timeout") { runner in
            let command = Data("@cmd:timeout-probe".utf8)
            runner.startExpectation(
                type: ProtocolValue.text,
                messages: [(Data("@report:timeout-probe".utf8), true)],
                timeout: 18
            )
            runner.messageAckDelay = 14
            runner.enqueueWrite(runner.chunks(
                for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
        })

        // Timeout goes last because the expected forced disconnect is the most disruptive case.
        suite.append(TestAction(name: "withhold-result-for-15s-timeout") { runner in
            let command = Data("@cmd:timeout-probe".utf8)
            runner.startExpectation(type: ProtocolValue.text,
                                    messages: [(Data("@report:timeout-probe".utf8), true)],
                                    timeout: 22)
            runner.expectedMessages = [(Data("@report:timeout-probe".utf8), ack: true)]
            runner.expectingDisconnect = true
            runner.disconnectEarliest = 14.5
            runner.disconnectLatest = 18.5
            runner.continueAfterDisconnect = false
            runner.withholdMessageAck = true
            runner.enqueueWrite(runner.chunks(
                for: frame(type: ProtocolValue.text, payload: command), fragmentation: .normal))
            // receivedMessage detects this final case and intentionally withholds its ACK.
        })

        return suite
    }

    private func finishSuite() {
        let elapsed = Date().timeIntervalSince(startedAt)
        log(event: "suite-complete", fields: [
            "passed": passed,
            "total": actions.count,
            "connections": connectionCount,
            "seconds": String(format: "%.3f", elapsed)
        ])
        fflush(stdout)
        Darwin.exit(passed == actions.count ? 0 : 1)
    }

    private func log(event: String, fields: [String: Any] = [:]) {
        var object = fields
        object["event"] = event
        object["timestamp"] = ISO8601DateFormatter().string(from: Date())
        if let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]),
           let line = String(data: data, encoding: .utf8) {
            print(line)
            fflush(stdout)
        }
    }
}

private let runner = HardwareRunner()
RunLoop.main.run()
