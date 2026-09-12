import Foundation
import os

private let parserLog = Logger(subsystem: "EasyBLE", category: "parser")

package final class StreamParser {
    private var buffer = Data()
    private var messageType: UInt8?
    private var messageLength = 0
    private var payload = Data()

    package var onMessage: ((UInt8, Data) -> Void)?
    package var onChunk: (() -> Void)?
    package var onResult: ((UInt8) -> Void)?
    package var onAck: (() -> Void)?
    package var onChannelHeader: ((Data) -> Void)?
    package var onChannelEvent: ((EasyBLEChannelEvent) -> Void)?
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

            case EasyBLEProtocol.ack:
                buffer.removeFirst(1)
                onAck?()

            case EasyBLEProtocol.begin:
                guard messageType == nil else {
                    parserLog.error("begin while message already open")
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
                    parserLog.error("invalid begin length=\(length)")
                    onError?()
                    return
                }
                guard chunkLength > 0,
                      chunkLength <= EasyBLEProtocol.chunkPayloadSize,
                      chunkLength <= Int(length) else {
                    parserLog.error("invalid begin chunkLength=\(chunkLength) length=\(length)")
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
                    parserLog.error("continue without begin")
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
                    parserLog.error("invalid continue chunkLength=\(chunkLength) remaining=\(remaining)")
                    onError?()
                    return
                }
                let frameLength = 3 + chunkLength
                guard buffer.count >= frameLength else { return }
                payload.append(buffer[start.advanced(by: 3)..<start.advanced(by: frameLength)])
                buffer.removeFirst(frameLength)
                finishChunk()

            case EasyBLEProtocol.data:
                guard buffer.count >= EasyBLEProtocol.dataHeaderSize else { return }
                let start = buffer.startIndex
                let flags = buffer[start.advanced(by: 1)]
                let length = Int(UInt16(buffer[start.advanced(by: 2)])
                    | (UInt16(buffer[start.advanced(by: 3)]) << 8))
                guard length <= EasyBLEProtocol.channelMaxPayload else {
                    parserLog.error("invalid data length=\(length)")
                    onError?()
                    return
                }
                let frameLength = EasyBLEProtocol.dataHeaderSize + length
                guard buffer.count >= frameLength else { return }
                let payload = Data(buffer[start.advanced(by: EasyBLEProtocol.dataHeaderSize)..<start.advanced(by: frameLength)])
                buffer.removeFirst(frameLength)
                guard deliverChannelFrame(flags: flags, payload: payload) else {
                    onError?()
                    return
                }

            default:
                parserLog.error("unknown opcode \(self.buffer[self.buffer.startIndex])")
                onError?()
                return
            }
        }
    }

    private func deliverChannelFrame(flags: UInt8, payload: Data) -> Bool {
        if flags & EasyBLEProtocol.dataFlagHeader != 0 {
            onChannelHeader?(payload)
            return true
        }
        var body = payload
        if flags & EasyBLEProtocol.dataFlagGap != 0 {
            guard body.count >= EasyBLEProtocol.gapPrefixSize else {
                parserLog.error("gap frame too short length=\(body.count)")
                return false
            }
            let start = body.startIndex
            let dropped = Int(body[start]) | (Int(body[start.advanced(by: 1)]) << 8)
            body = Data(body.dropFirst(EasyBLEProtocol.gapPrefixSize))
            onChannelEvent?(.gap(droppedBytes: dropped))
        }
        if !body.isEmpty {
            onChannelEvent?(.data(body))
        }
        if flags & EasyBLEProtocol.dataFlagEnd != 0 {
            onChannelEvent?(.ended)
        }
        return true
    }

    private func finishChunk() {
        guard let type = messageType else { return }
        guard payload.count == messageLength else {
            onChunk?()
            return
        }
        let data = payload
        messageType = nil
        messageLength = 0
        payload = Data()
        onMessage?(type, data)
    }
}
