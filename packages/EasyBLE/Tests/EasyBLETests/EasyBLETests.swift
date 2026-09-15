import Foundation
import Testing
@testable import EasyBLE

private func allFrames(type: EasyBLEMessageType, payload: Data) -> [Data] {
    var frames: [Data] = []
    var offset = 0
    while offset < payload.count {
        frames.append(EasyBLEProtocol.frame(type: type, payload: payload, offset: offset))
        offset += min(EasyBLEProtocol.chunkPayloadSize, payload.count - offset)
    }
    return frames
}

private func dataFrame(flags: UInt8, payload: Data) -> Data {
    var frame = Data([EasyBLEProtocol.data, flags, UInt8(payload.count & 0xFF), UInt8(payload.count >> 8)])
    frame.append(payload)
    return frame
}

@Test func parserReassemblesSplitFrames() {
    let payload = Data((0..<4_000).map { UInt8($0 & 0xFF) })
    let framed = allFrames(type: .image, payload: payload).reduce(into: Data()) { $0.append($1) }
    let parser = StreamParser()
    var got: Data?
    parser.onMessage = { _, data in got = data }
    parser.append(framed.prefix(10))
    #expect(got == nil)
    parser.append(Data(framed.dropFirst(10)))
    #expect(got == payload)
}

@Test func parserReadsResult() {
    let parser = StreamParser()
    var status: UInt8?
    parser.onResult = { status = $0 }
    parser.append(EasyBLEProtocol.resultFrame(true))
    #expect(status == 1)
}

@Test func parserReadsResultBetweenChunks() {
    let payload = Data((0..<4_000).map { UInt8($0 & 0xFF) })
    let frames = allFrames(type: .image, payload: payload)
    #expect(frames.count == 2)
    let parser = StreamParser()
    var status: UInt8?
    var got: Data?
    parser.onResult = { status = $0 }
    parser.onMessage = { _, data in got = data }
    parser.append(frames[0])
    parser.append(EasyBLEProtocol.resultFrame(true))
    parser.append(frames[1])
    #expect(status == 1)
    #expect(got == payload)
}

@Test func parserReportsChunkBoundariesBeforeFinalChunk() {
    let payload = Data((0..<7_000).map { UInt8($0 & 0xFF) })
    let frames = allFrames(type: .image, payload: payload)
    #expect(frames.count == 3)
    let parser = StreamParser()
    var chunks = 0
    var got: Data?
    parser.onChunk = { chunks += 1 }
    parser.onMessage = { _, data in got = data }
    for frame in frames {
        parser.append(frame)
    }
    #expect(chunks == 2)
    #expect(got == payload)
}

@Test func parserReadsAckBetweenChunks() {
    let payload = Data((0..<4_000).map { UInt8($0 & 0xFF) })
    let frames = allFrames(type: .text, payload: payload)
    let parser = StreamParser()
    var acks = 0
    var got: Data?
    parser.onAck = { acks += 1 }
    parser.onMessage = { _, data in got = data }
    parser.append(frames[0])
    parser.append(EasyBLEProtocol.ackFrame)
    parser.append(frames[1])
    #expect(acks == 1)
    #expect(got == payload)
}

@Test func parserReportsUnknownOpcode() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(Data([0xFF]))
    #expect(failed)
}

@Test func offerFrameEncodesTypeAndLength() {
    let frame = EasyBLEProtocol.offerFrame(type: .image, length: 7034)
    #expect(frame.count == 6)
    #expect(frame[0] == EasyBLEProtocol.offer)
    #expect(frame[1] == EasyBLEMessageType.image.rawValue)
    let length = UInt32(frame[2])
        | (UInt32(frame[3]) << 8)
        | (UInt32(frame[4]) << 16)
        | (UInt32(frame[5]) << 24)
    #expect(length == 7034)
}

@Test func parserReportsContinueWithoutBegin() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(Data([EasyBLEProtocol.continueOpcode, 0x01, 0x00, 0xAA]))
    #expect(failed)
}

@Test func parserDecodesChannelFramesSplitAcrossAppends() {
    let descriptor = Data([0x01, 0x01, 0x80, 0x3E, 0x00, 0x00, 0x00, 0x01])
    let payload = Data((0..<240).map { UInt8($0 & 0xFF) })
    var stream = dataFrame(flags: EasyBLEProtocol.dataFlagHeader, payload: descriptor)
    stream.append(dataFrame(flags: 0, payload: payload))
    stream.append(dataFrame(flags: EasyBLEProtocol.dataFlagGap, payload: Data([0x10, 0x00]) + payload.prefix(100)))
    stream.append(dataFrame(flags: EasyBLEProtocol.dataFlagEnd, payload: Data()))

    let parser = StreamParser()
    var header: Data?
    var events: [EasyBLEChannelEvent] = []
    parser.onChannelHeader = { header = $0 }
    parser.onChannelEvent = { events.append($0) }
    for byte in stream {
        parser.append(Data([byte]))
    }

    #expect(header == descriptor)
    #expect(events.count == 4)
    if case .data(let data) = events[0] { #expect(data == payload) } else { Issue.record("expected data") }
    if case .gap(let dropped) = events[1] { #expect(dropped == 16) } else { Issue.record("expected gap") }
    if case .data(let data) = events[2] { #expect(data == payload.prefix(100)) } else { Issue.record("expected data") }
    if case .ended = events[3] {} else { Issue.record("expected ended") }
}

@Test func parserReadsChannelFrameBetweenChunks() {
    let payload = Data((0..<4_000).map { UInt8($0 & 0xFF) })
    let frames = allFrames(type: .image, payload: payload)
    let parser = StreamParser()
    var channelEvents = 0
    var got: Data?
    parser.onChannelEvent = { _ in channelEvents += 1 }
    parser.onMessage = { _, data in got = data }
    parser.append(frames[0])
    parser.append(dataFrame(flags: 0, payload: Data(repeating: 0xAB, count: 240)))
    parser.append(frames[1])
    #expect(channelEvents == 1)
    #expect(got == payload)
}

@Test func parserRejectsOversizedDataFrame() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(Data([EasyBLEProtocol.data, 0x00, 0xF1, 0x00]))
    #expect(failed)
}

@Test func parserRejectsShortGapFrame() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(dataFrame(flags: EasyBLEProtocol.dataFlagGap, payload: Data([0x01])))
    #expect(failed)
}

@Test func controlFrameEncodesEnable() {
    #expect(EasyBLEProtocol.controlFrame(true) == Data([0x08, 0x01]))
    #expect(EasyBLEProtocol.controlFrame(false) == Data([0x08, 0x00]))
}

@Test func incomingChannelFansOutAndEnds() async {
    var controls: [Bool] = []
    let channel = EasyBLEIncomingChannel(descriptor: Data()) { _, enabled in
        controls.append(enabled)
    }
    let first = channel.events
    let second = channel.bytes
    channel.accept()
    channel.accept()
    channel.deliver(.gap(droppedBytes: 480))
    channel.deliver(.data(Data([1, 2, 3])))
    channel.deliver(.ended)
    channel.deliver(.data(Data([4])))

    var firstEvents: [EasyBLEChannelEvent] = []
    for await event in first {
        firstEvents.append(event)
    }
    var secondBytes: [Data] = []
    for await data in second {
        secondBytes.append(data)
    }

    #expect(controls == [true, false])
    #expect(firstEvents.count == 3)
    if case .gap(let dropped) = firstEvents[0] { #expect(dropped == 480) } else { Issue.record("expected gap") }
    if case .data(let data) = firstEvents[1] { #expect(data == Data([1, 2, 3])) } else { Issue.record("expected data") }
    if case .ended = firstEvents[2] {} else { Issue.record("expected ended") }
    #expect(secondBytes == [Data([1, 2, 3])])
    #expect(channel.isEnded)
    channel.close()
    #expect(controls == [true, false])
}

@Test func incomingChannelAcknowledgesDeviceEndOnce() {
    var controls: [Bool] = []
    let channel = EasyBLEIncomingChannel(descriptor: Data()) { _, enabled in
        controls.append(enabled)
    }
    channel.deliver(.ended)
    channel.deliver(.ended)
    channel.close()
    #expect(controls == [false])
    #expect(channel.isEnded)
}

@Test func incomingChannelDoesNotAcknowledgeAfterLocalClose() {
    var controls: [Bool] = []
    let channel = EasyBLEIncomingChannel(descriptor: Data()) { _, enabled in
        controls.append(enabled)
    }
    channel.accept()
    channel.close()
    channel.deliver(.ended)
    #expect(controls == [true, false])
}

@Test func incomingChannelCloseSendsControlOnce() {
    var controls: [Bool] = []
    let channel = EasyBLEIncomingChannel(descriptor: Data()) { _, enabled in
        controls.append(enabled)
    }
    channel.close()
    channel.accept()
    channel.close()
    #expect(controls == [false])
    #expect(channel.isEnded)
    #expect(!channel.isAccepted)
}

@Test func incomingChannelDropsOldestWhenConsumerStalls() async {
    let channel = EasyBLEIncomingChannel(descriptor: Data()) { _, _ in }
    let stream = channel.events
    for index in 0..<(EasyBLEIncomingChannel.bufferedEventLimit + 10) {
        channel.deliver(.data(Data([UInt8(index & 0xFF)])))
    }
    channel.end()
    var count = 0
    for await _ in stream {
        count += 1
    }
    #expect(count == EasyBLEIncomingChannel.bufferedEventLimit)
}
