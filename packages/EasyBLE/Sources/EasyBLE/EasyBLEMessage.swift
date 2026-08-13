import Foundation

public enum EasyBLEMessageType: UInt8, Sendable {
    case text = 0x01
    case image = 0x02
}

public struct EasyBLEMessage: Sendable {
    public var type: EasyBLEMessageType
    public var data: Data
}
