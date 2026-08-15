import Foundation

package enum EasyBLEProtocol {
    package static let serviceUUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let phoneToDeviceUUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let deviceToPhoneUUID = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E"
    package static let restoreIdentifier = "EasyBLE"
    package static let result: UInt8 = 0x02
    package static let begin: UInt8 = 0x03
    package static let continueOpcode: UInt8 = 0x04
    package static let chunkPayloadSize = 3_072
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
}
