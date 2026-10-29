import Foundation

// The phone link's byte layer (docs/ble-link.md, "Framing"): one protobuf
// message split into fragments, one ATT write or notification each.
//
//   byte 0   b7 FIRST, b6 LAST, b5-b4 reserved, b3-b0 fragment counter
//   byte 1.. the next piece of the encoded message
//
// Pure Foundation, no CoreBluetooth or SwiftProtobuf, so it compiles on its
// own with a test driver (tools/link_framing_test/run.sh).

enum LinkFrame {
    static let first: UInt8 = 0x80
    static let last: UInt8 = 0x40
    static let counterMask: UInt8 = 0x0F

    /// Split one encoded message into fragments of at most `payloadMax` bytes
    /// of message each (MTU - 3 - 1), numbering them from `counter`, which is
    /// left at the next fragment's number. An empty message is one fragment.
    static func fragments(_ message: Data, payloadMax: Int, counter: inout UInt8) -> [Data] {
        let size = max(1, payloadMax)
        var out: [Data] = []
        var off = message.startIndex
        repeat {
            let end = min(off + size, message.endIndex)
            var header = counter & counterMask
            if off == message.startIndex { header |= first }
            if end == message.endIndex { header |= last }
            var frag = Data(capacity: 1 + end - off)
            frag.append(header)
            frag.append(message[off..<end])
            out.append(frag)
            counter = (counter &+ 1) & counterMask
            off = end
        } while off < message.endIndex
        return out
    }
}

/// Rebuilds messages from fragments. A gap in the counter, a FIRST in the
/// middle of a message or a continuation without a FIRST drops the partial
/// message (reported through `problem`); the next FIRST starts clean.
struct LinkReassembler {
    /// Counter the next fragment should carry; nil until the first one.
    private(set) var expected: UInt8?
    private var partial: Data?
    /// Largest message accepted; a longer one is dropped.
    var maxMessage = 64 * 1024

    struct Output {
        var message: Data?
        var problem: String?
    }

    mutating func reset() {
        expected = nil
        partial = nil
    }

    mutating func feed(_ fragment: Data) -> Output {
        guard let header = fragment.first else { return Output(problem: "empty fragment") }
        let counter = header & LinkFrame.counterMask
        let isFirst = header & LinkFrame.first != 0
        let isLast = header & LinkFrame.last != 0
        let body = fragment.dropFirst()
        var problem: String?
        if let want = expected, want != counter {
            problem = "counter gap: want \(want) got \(counter)"
            partial = nil
        }
        expected = (counter &+ 1) & LinkFrame.counterMask
        if isFirst {
            if partial != nil {
                problem = problem ?? "FIRST inside a message"
            }
            partial = Data(body)
        } else if partial == nil {
            return Output(problem: problem ?? "continuation without FIRST")
        } else {
            partial?.append(body)
        }
        if let p = partial, p.count > maxMessage {
            partial = nil
            return Output(problem: "message over \(maxMessage) bytes")
        }
        guard isLast, let done = partial else { return Output(problem: problem) }
        partial = nil
        return Output(message: done, problem: problem)
    }
}

/// CRC-32 (IEEE 802.3, reflected, as zlib `crc32`), incremental:
/// `update(update(0, a), b) == checksum(a + b)`.
enum CRC32 {
    private static let table: [UInt32] = (0..<256).map { i in
        var c = UInt32(i)
        for _ in 0..<8 { c = c & 1 != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
        return c
    }

    static func update(_ crc: UInt32, _ data: Data) -> UInt32 {
        var c = ~crc
        data.withUnsafeBytes { (buf: UnsafeRawBufferPointer) in
            for b in buf { c = table[Int((c ^ UInt32(b)) & 0xFF)] ^ (c >> 8) }
        }
        return ~c
    }

    static func checksum(_ data: Data) -> UInt32 { update(0, data) }
}
