// Standalone Kronos Object Dump Request/capture, for reverse-engineering work
// from the terminal without touching the SynthEdit app — companion to
// moog_dump.swift, same shape, different device/protocol.
//
// Sends an Object Dump Request (func 0x72) for a Program (obj=0x00) at a
// given bank+index, per KRONOS_MIDI_SysEx.txt ("[72] Object Dump Request"):
//   F0 42 3g 68 72 <obj> <bank> <idH> <idL> F7
// and saves the first matching Object Dump reply (func 0x73, same mfrId/
// familyId) to a raw .syx file.
//
// usage: kronos_dump <destination-name-substring> <bankHex> <indexDecimal> <output.syx> [channel0based] [timeoutSeconds]
//   e.g.: kronos_dump kronos 41 0 /tmp/ub000.syx 0 3
//         (bank 0x41 = USER-B, index 0 = first slot, channel 0)
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 5 else {
    err("usage: kronos_dump <destination-name-substring> <bankHex> <indexDecimal> <output.syx> [channel0based] [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
guard let bank = UInt8(CommandLine.arguments[2], radix: 16) else {
    err("bank must be hex, e.g. 41 for USER-B")
    exit(1)
}
guard let index = UInt16(CommandLine.arguments[3]) else {
    err("index must be decimal (0-based)")
    exit(1)
}
let outputPath = CommandLine.arguments[4]
let channel = CommandLine.arguments.count >= 6 ? (UInt8(CommandLine.arguments[5]) ?? 0) : 0
let timeout = CommandLine.arguments.count >= 7 ? (Double(CommandLine.arguments[6]) ?? 3.0) : 3.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_dump" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var capturedData: [UInt8]? = nil
var packetsSeen = 0
var assembling: [UInt8] = []

var inPort = MIDIPortRef()
let inPortStatus = MIDIInputPortCreateWithBlock(client, "in" as CFString, &inPort) { pktListPtr, _ in
    var packet = pktListPtr.pointee.packet
    let count = pktListPtr.pointee.numPackets
    for i in 0..<count {
        let bytes = withUnsafeBytes(of: packet.data) { raw -> [UInt8] in
            Array(raw.prefix(Int(packet.length)))
        }
        lock.lock()
        packetsSeen += 1
        if capturedData == nil {
            for b in bytes {
                if b == 0xF0 {
                    assembling = [b]
                } else if b >= 0xF8 {
                    // System Real-Time — spec-legal mid-SysEx, must not be appended (see kronos_dump_diff.py's notes)
                } else if !assembling.isEmpty {
                    assembling.append(b)
                    if b == 0xF7 {
                        // F0 42 3g 68 73 obj bank idH idL version ... F7
                        if assembling.count > 6, assembling[1] == 0x42, assembling[3] == 0x68, assembling[4] == 0x73 {
                            capturedData = assembling
                        }
                        assembling = []
                    }
                }
            }
        }
        lock.unlock()
        if i + 1 < count {
            packet = MIDIPacketNext(&packet).pointee
        }
    }
}
check(inPortStatus, "MIDIInputPortCreateWithBlock")

func findEndpoint(matching substring: String, source: Bool) -> MIDIEndpointRef? {
    let count = source ? MIDIGetNumberOfSources() : MIDIGetNumberOfDestinations()
    for i in 0..<count {
        let ep = source ? MIDIGetSource(i) : MIDIGetDestination(i)
        var cfName: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cfName)
        if let name = cfName?.takeRetainedValue() as String?, name.lowercased().contains(substring) {
            return ep
        }
    }
    return nil
}

guard let destination = findEndpoint(matching: portNameSubstring, source: false) else {
    err("no MIDI destination matching '\(portNameSubstring)'")
    exit(1)
}
guard let source = findEndpoint(matching: portNameSubstring, source: true) else {
    err("no MIDI source matching '\(portNameSubstring)'")
    exit(1)
}

check(MIDIPortConnectSource(inPort, source, nil), "MIDIPortConnectSource")

let idH = UInt8((index >> 7) & 0x7F)
let idL = UInt8(index & 0x7F)
let sysex: [UInt8] = [0xF0, 0x42, 0x30 | (channel & 0x0F), 0x68, 0x72, 0x00, bank, idH, idL, 0xF7]

let bufSize = 1024
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)

check(MIDISend(outPort, destination, listPtr), "MIDISend")
print("sent Object Dump Request: obj=00 bank=\(String(format: "%02X", bank)) index=\(index) (idH=\(idH) idL=\(idL))")

let deadline = Date().addingTimeInterval(timeout)
while Date() < deadline {
    lock.lock()
    let done = capturedData != nil
    lock.unlock()
    if done { break }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}

lock.lock()
let result = capturedData
let seen = packetsSeen
lock.unlock()

guard let data = result else {
    err("timed out waiting for a reply (saw \(seen) unrelated MIDI packet(s))")
    exit(2)
}

try! Data(data).write(to: URL(fileURLWithPath: outputPath))
print("wrote \(data.count) bytes to \(outputPath)")
