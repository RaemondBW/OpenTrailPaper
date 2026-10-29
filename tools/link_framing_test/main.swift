import Foundation

// Cases for companion-ios/Sources/LinkFraming.swift (docs/ble-link.md, Framing).

var failures = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    if !ok { failures += 1; print("FAIL line \(line): \(what)") }
}

func bytes(_ n: Int, seed: UInt8 = 1) -> Data {
    Data((0..<n).map { UInt8(truncatingIfNeeded: $0 &* 31 &+ Int(seed)) })
}

// CRC-32 IEEE check value and zlib agreement.
check(CRC32.checksum(Data("123456789".utf8)) == 0xCBF4_3926, "crc check value")
check(CRC32.checksum(Data()) == 0, "crc of nothing")
let big = bytes(100_000)
check(CRC32.update(CRC32.update(0, big.prefix(12_345)), big.dropFirst(12_345)) == CRC32.checksum(big),
      "crc incremental")
check(CRC32.checksum(Data("The quick brown fox jumps over the lazy dog".utf8)) == 0x414F_A339, "crc fox")

// One fragment: FIRST|LAST and the counter.
do {
    var c: UInt8 = 5
    let f = LinkFrame.fragments(Data([1, 2, 3]), payloadMax: 243, counter: &c)
    check(f.count == 1 && f[0] == Data([0xC5, 1, 2, 3]), "single fragment header")
    check(c == 6, "counter advanced")
}

// Empty message: one fragment, header only.
do {
    var c: UInt8 = 0
    let f = LinkFrame.fragments(Data(), payloadMax: 20, counter: &c)
    check(f == [Data([0xC0])], "empty message")
}

// Split, counter wrap, exact multiples, reassembly round trip.
for size in [1, 19, 20, 21, 40, 243, 244, 1000, 4096] {
    for pm in [19, 20, 243] {
        var c: UInt8 = 14
        let msg = bytes(size, seed: UInt8(size & 0xFF))
        let frags = LinkFrame.fragments(msg, payloadMax: pm, counter: &c)
        check(frags.count == max(1, (size + pm - 1) / pm), "fragment count \(size)/\(pm)")
        check(frags.allSatisfy { $0.count <= pm + 1 }, "fragment size \(size)/\(pm)")
        check(frags.first![0] & 0x80 != 0 && frags.last![0] & 0x40 != 0, "first/last bits")
        check(frags.dropFirst().allSatisfy { $0[0] & 0x80 == 0 }, "FIRST only once")
        check(frags.dropLast().allSatisfy { $0[0] & 0x40 == 0 }, "LAST only once")
        check(frags.allSatisfy { $0[0] & 0x30 == 0 }, "reserved bits zero")
        var r = LinkReassembler()
        var got: [Data] = []
        for f in frags {
            let o = r.feed(f)
            check(o.problem == nil, "no problem \(size)/\(pm): \(o.problem ?? "")")
            if let m = o.message { got.append(m) }
        }
        check(got == [msg], "round trip \(size)/\(pm)")
    }
}

// Several messages back to back on one counter.
do {
    var c: UInt8 = 0
    let msgs = (1...40).map { bytes($0 * 13, seed: UInt8($0)) }
    var r = LinkReassembler()
    var got: [Data] = []
    for m in msgs {
        for f in LinkFrame.fragments(m, payloadMax: 50, counter: &c) {
            let o = r.feed(f)
            check(o.problem == nil, "stream problem \(o.problem ?? "")")
            if let d = o.message { got.append(d) }
        }
    }
    check(got == msgs, "stream of messages")
}

// A gap drops the partial message; the next FIRST starts clean.
do {
    var c: UInt8 = 0
    let a = LinkFrame.fragments(bytes(100), payloadMax: 30, counter: &c)   // 4 fragments
    let b = LinkFrame.fragments(bytes(10, seed: 9), payloadMax: 30, counter: &c)
    var r = LinkReassembler()
    _ = r.feed(a[0]); _ = r.feed(a[1])
    let skip = r.feed(a[3])   // a[2] lost
    check(skip.message == nil && skip.problem != nil, "gap reported")
    let o = r.feed(b[0])
    check(o.message == bytes(10, seed: 9) && o.problem == nil, "clean after gap")
}

// Continuation without FIRST.
do {
    var c: UInt8 = 3
    let a = LinkFrame.fragments(bytes(100), payloadMax: 30, counter: &c)
    var r = LinkReassembler()
    let o = r.feed(a[1])
    check(o.message == nil && o.problem != nil, "continuation without FIRST")
    _ = r.feed(a[2]); _ = r.feed(a[3])
    var c2: UInt8 = 7
    let b = LinkFrame.fragments(bytes(5), payloadMax: 30, counter: &c2)
    check(r.feed(b[0]).message == bytes(5), "next FIRST after stray continuation")
}

// FIRST in the middle of a message (counter in sequence): partial dropped,
// the new one kept.
do {
    var c: UInt8 = 0
    let a = LinkFrame.fragments(bytes(100), payloadMax: 30, counter: &c)
    let b = LinkFrame.fragments(bytes(20, seed: 4), payloadMax: 30, counter: &c)
    var r = LinkReassembler()
    _ = r.feed(a[0])
    // b[0] carries counter 4; renumber it to 1 so only the FIRST rule fires.
    var bad = b[0]; bad[0] = (bad[0] & 0xF0) | 1
    let o = r.feed(bad)
    check(o.problem != nil && o.message == bytes(20, seed: 4), "FIRST mid-message")
}

// Oversized message dropped.
do {
    var c: UInt8 = 0
    var r = LinkReassembler()
    r.maxMessage = 100
    var last = LinkReassembler.Output()
    for f in LinkFrame.fragments(bytes(300), payloadMax: 50, counter: &c) { last = r.feed(f) }
    check(last.message == nil, "oversized dropped")
}

print(failures == 0 ? "link framing: all passed" : "link framing: \(failures) FAILED")
exit(failures == 0 ? 0 : 1)
