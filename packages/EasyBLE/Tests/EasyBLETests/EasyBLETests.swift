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

@Test func parserReportsUnknownOpcode() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(Data([0xFF]))
    #expect(failed)
}

@Test func parserReportsContinueWithoutBegin() {
    let parser = StreamParser()
    var failed = false
    parser.onError = { failed = true }
    parser.append(Data([EasyBLEProtocol.continueOpcode, 0x01, 0x00, 0xAA]))
    #expect(failed)
}
