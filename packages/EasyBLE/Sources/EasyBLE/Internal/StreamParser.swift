import Foundation

package final class StreamParser {
    private var buffer = Data()
    private var messageType: UInt8?
    private var messageLength = 0
    private var payload = Data()

    package var onMessage: ((UInt8, Data) -> Void)?
    package var onResult: ((UInt8) -> Void)?
    package var onError: (() -> Void)?

    package init() {}

    package func reset() {
        buffer.removeAll(keepingCapacity: true)
        messageType = nil
        messageLength = 0
        payload.removeAll(keepingCapacity: false)
    }

    package func append(_ data: Data) {
        buffer.append(data)
        parse()
    }

    private func parse() {
        while !buffer.isEmpty {
            switch buffer[buffer.startIndex] {
            case EasyBLEProtocol.result:
                guard buffer.count >= 2 else { return }
                let status = buffer[buffer.startIndex.advanced(by: 1)]
                buffer.removeFirst(2)
                onResult?(status)

            case EasyBLEProtocol.begin:
                guard messageType == nil else {
                    onError?()
                    return
                }
                guard buffer.count >= 8 else { return }
                let start = buffer.startIndex
                let type = buffer[start.advanced(by: 1)]
                let length = UInt32(buffer[start.advanced(by: 2)])
                    | (UInt32(buffer[start.advanced(by: 3)]) << 8)
                    | (UInt32(buffer[start.advanced(by: 4)]) << 16)
                    | (UInt32(buffer[start.advanced(by: 5)]) << 24)
                let chunkLength = Int(UInt16(buffer[start.advanced(by: 6)])
                    | (UInt16(buffer[start.advanced(by: 7)]) << 8))
                guard length > 0, length <= EasyBLEProtocol.maxMessageSize else {
                    onError?()
                    return
                }
                guard chunkLength > 0,
                      chunkLength <= EasyBLEProtocol.chunkPayloadSize,
                      chunkLength <= Int(length) else {
                    onError?()
                    return
                }
                let frameLength = 8 + chunkLength
                guard buffer.count >= frameLength else { return }
                messageType = type
                messageLength = Int(length)
                payload = Data(buffer[start.advanced(by: 8)..<start.advanced(by: frameLength)])
                buffer.removeFirst(frameLength)
                finishChunk()

            case EasyBLEProtocol.continueOpcode:
                guard messageType != nil else {
                    onError?()
                    return
                }
                guard buffer.count >= 3 else { return }
                let start = buffer.startIndex
                let chunkLength = Int(UInt16(buffer[start.advanced(by: 1)])
                    | (UInt16(buffer[start.advanced(by: 2)]) << 8))
                let remaining = messageLength - payload.count
                guard chunkLength > 0,
                      chunkLength <= EasyBLEProtocol.chunkPayloadSize,
                      chunkLength <= remaining else {
                    onError?()
                    return
                }
                let frameLength = 3 + chunkLength
                guard buffer.count >= frameLength else { return }
                payload.append(buffer[start.advanced(by: 3)..<start.advanced(by: frameLength)])
                buffer.removeFirst(frameLength)
                finishChunk()

            default:
                onError?()
                return
            }
        }
    }

    private func finishChunk() {
        guard payload.count == messageLength, let type = messageType else { return }
        let data = payload
        messageType = nil
        messageLength = 0
        payload = Data()
        onMessage?(type, data)
    }
}
