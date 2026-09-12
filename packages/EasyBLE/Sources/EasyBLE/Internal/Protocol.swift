import Foundation

package enum EasyBLEProtocol {
    package static let serviceUUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let phoneToDeviceUUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let deviceToPhoneUUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let restoreIdentifier = "EasyBLE"
    package static let result: UInt8 = 0x02
    package static let begin: UInt8 = 0x03
    package static let continueOpcode: UInt8 = 0x04
    package static let offer: UInt8 = 0x05
    package static let ack: UInt8 = 0x06
    package static let data: UInt8 = 0x07
    package static let control: UInt8 = 0x08
    package static let ackFrame = Data([ack])
    package static let dataFlagHeader: UInt8 = 0x01
    package static let dataFlagGap: UInt8 = 0x02
    package static let dataFlagEnd: UInt8 = 0x04
    package static let dataHeaderSize = 4
    package static let gapPrefixSize = 2
    package static let channelMaxPayload = 240
    package static let chunkPayloadSize = 3_072
    package static let offerThreshold = chunkPayloadSize
    package static let maxMessageSize = 8 * 1024 * 1024
    package static let resultTimeout: TimeInterval = 15

    package static func frame(type: EasyBLEMessageType, payload: Data, offset: Int) -> Data {
        let chunkLength = min(chunkPayloadSize, payload.count - offset)
        var frame = Data()
        if offset == 0 {
            frame.append(begin)
            frame.append(type.rawValue)
            var length = UInt32(payload.count).littleEndian
            withUnsafeBytes(of: &length) { frame.append(contentsOf: $0) }
        } else {
            frame.append(continueOpcode)
        }
        var littleEndianChunkLength = UInt16(chunkLength).littleEndian
        withUnsafeBytes(of: &littleEndianChunkLength) { frame.append(contentsOf: $0) }
        frame.append(payload.subdata(in: offset..<(offset + chunkLength)))
        return frame
    }

    package static func resultFrame(_ success: Bool) -> Data {
        Data([result, success ? 1 : 0])
    }

    package static func offerFrame(type: EasyBLEMessageType, length: Int) -> Data {
        var frame = Data([offer, type.rawValue])
        var littleEndianLength = UInt32(length).littleEndian
        withUnsafeBytes(of: &littleEndianLength) { frame.append(contentsOf: $0) }
        return frame
    }

    package static func controlFrame(_ enabled: Bool) -> Data {
        Data([control, enabled ? 1 : 0])
    }
}
