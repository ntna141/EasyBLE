import Foundation

enum EasyBLEProtocol {
    static let serviceUUID = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E"
    static let streamUUID = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E"
    static let restoreIdentifier = "EasyBLE"
    static let result: UInt8 = 0x02
    static let begin: UInt8 = 0x03
    static let continueOpcode: UInt8 = 0x04
    static let chunkPayloadSize = 3_072
    static let maxMessageSize = 8 * 1024 * 1024
    static let resultTimeout: TimeInterval = 15

    static func frame(type: EasyBLEMessageType, payload: Data, offset: Int) -> Data {
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

    static func resultFrame(_ success: Bool) -> Data {
        Data([result, success ? 1 : 0])
    }
}
