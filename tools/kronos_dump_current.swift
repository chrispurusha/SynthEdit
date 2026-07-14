// Standalone Kronos Current Object Dump Request/capture — the LIVE EDIT
// BUFFER (whatever's currently loaded/being edited), not a specific stored
// bank+index — companion to kronos_dump.swift (which is bank-addressed).
// This is what actually reflects a just-sent Parameter Change (func 0x43),
// since Parameter Change edits the live buffer, not stored memory.
//
// Sends func 0x74 per KRONOS_MIDI_SysEx.txt ("[74] Current Object Dump
// Request"): F0 42 3g 68 74 <obj> F7, and saves the first matching
// Current Object Dump reply (func 0x75) to a raw .syx file.
//
// usage: kronos_dump_current <destination-name-substring> <objHex> <output.syx> [channel0based] [timeoutSeconds]
//   e.g.: kronos_dump_current kronos 00 /tmp/current.syx 0 3   (obj=00 Program)
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 4 else {
    err("usage: kronos_dump_current <destination-name-substring> <objHex> <output.syx> [channel0based] [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
guard let obj = UInt8(CommandLine.arguments[2], radix: 16) else {
    err("obj must be hex, e.g. 00 for Program")
    exit(1)
}
let outputPath = CommandLine.arguments[3]
let channel = CommandLine.arguments.count >= 5 ? (UInt8(CommandLine.arguments[4]) ?? 0) : 0
let timeout = CommandLine.arguments.count >= 6 ? (Double(CommandLine.arguments[5]) ?? 3.0) : 3.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_dump_current" as CFString, nil, nil, &client), "MIDIClientCreate")

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
                    // System Real-Time (Clock/Start/Stop/...) — spec-legal to
                    // interleave mid-SysEx from a device with its clock
                    // running; must NOT be appended to the message being
                    // assembled (found 2026-07-14: an interleaved 0xF8 Timing
                    // Clock byte was silently corrupting captures by exactly
                    // one byte, cascading a false diff through the whole
                    // decoded payload — see kronos_dump_diff.py's own notes).
                } else if !assembling.isEmpty {
                    assembling.append(b)
                    if b == 0xF7 {
                        // F0 42 3g 68 75 obj version ... F7
                        if assembling.count > 5, assembling[1] == 0x42, assembling[3] == 0x68, assembling[4] == 0x75 {
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

let sysex: [UInt8] = [0xF0, 0x42, 0x30 | (channel & 0x0F), 0x68, 0x74, obj, 0xF7]

let bufSize = 1024
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)

check(MIDISend(outPort, destination, listPtr), "MIDISend")
print("sent Current Object Dump Request: obj=\(String(format: "%02X", obj))")

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
