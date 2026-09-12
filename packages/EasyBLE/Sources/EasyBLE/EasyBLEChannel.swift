import Foundation

public enum EasyBLEChannelEvent: Sendable {
    case data(Data)
    case gap(droppedBytes: Int)
    case ended
}

public final class EasyBLEIncomingChannel {
    public static let bufferedEventLimit = 256

    public let descriptor: Data
    public private(set) var isAccepted = false
    public private(set) var isEnded = false

    private let control: (EasyBLEIncomingChannel, Bool) -> Void
    private var continuations: [UUID: AsyncStream<EasyBLEChannelEvent>.Continuation] = [:]

    package init(descriptor: Data, control: @escaping (EasyBLEIncomingChannel, Bool) -> Void) {
        self.descriptor = descriptor
        self.control = control
    }

    public var events: AsyncStream<EasyBLEChannelEvent> {
        AsyncStream(bufferingPolicy: .bufferingNewest(Self.bufferedEventLimit)) { continuation in
            guard !isEnded else {
                continuation.finish()
                return
            }
            let id = UUID()
            continuations[id] = continuation
            continuation.onTermination = { [weak self] _ in
                Task { @MainActor in
                    self?.continuations[id] = nil
                }
            }
        }
    }

    public var bytes: AsyncStream<Data> {
        let source = events
        return AsyncStream(bufferingPolicy: .bufferingNewest(Self.bufferedEventLimit)) { continuation in
            let task = Task {
                for await event in source {
                    if case .data(let data) = event {
                        continuation.yield(data)
                    }
                }
                continuation.finish()
            }
            continuation.onTermination = { _ in task.cancel() }
        }
    }

    public func accept() {
        guard !isAccepted, !isEnded else { return }
        isAccepted = true
        control(self, true)
    }

    public func close() {
        guard !isEnded else { return }
        control(self, false)
        end()
    }

    package func deliver(_ event: EasyBLEChannelEvent) {
        guard !isEnded else { return }
        if case .ended = event {
            end()
            return
        }
        for continuation in continuations.values {
            continuation.yield(event)
        }
    }

    package func end() {
        guard !isEnded else { return }
        isEnded = true
        for continuation in continuations.values {
            continuation.yield(.ended)
            continuation.finish()
        }
        continuations.removeAll()
    }
}
