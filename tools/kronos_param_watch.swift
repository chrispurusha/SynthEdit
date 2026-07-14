// Like kronos_param.swift, but listens for ANY reply from the device
// (matching mfrId/familyId only, not a specific func) for a short window
// afterward and prints its raw bytes — specifically to catch a func 0x24
// "Reply" (with a non-zero Reply Code explaining a rejection) that
// kronos_dump_current.swift's own func-0x75-only filter would silently
// ignore.
//
// usage: kronos_param_watch <destination-name-substring> <TYP> <SOC> <SUB> <PID> <IDX> <value> [channel0based] [timeoutSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 8 else {
    err("usage: kronos_param_watch <destination-name-substring> <TYP> <SOC> <SUB> <PID> <IDX> <value> [channel0based] [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
guard let typ = UInt8(CommandLine.arguments[2]),
      let soc = UInt8(CommandLine.arguments[3]),
      let sub = UInt8(CommandLine.arguments[4]),
      let pid = UInt8(CommandLine.arguments[5]),
      let idx = UInt8(CommandLine.arguments[6]),
      let value = Int32(CommandLine.arguments[7]) else {
    err("TYP/SOC/SUB/PID/IDX/value must be decimal integers")
    exit(1)
}
let channel = CommandLine.arguments.count >= 9 ? (UInt8(CommandLine.arguments[8]) ?? 0) : 0
let timeout = CommandLine.arguments.count >= 10 ? (Double(CommandLine.arguments[9]) ?? 2.0) : 2.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_param_watch" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var replies: [[UInt8]] = []
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
        for b in bytes {
            if b == 0xF0 {
                assembling = [b]
            } else if b >= 0xF8 {
                // System Real-Time — spec-legal mid-SysEx, must not be appended (see kronos_dump_diff.py's notes)
            } else if !assembling.isEmpty {
                assembling.append(b)
                if b == 0xF7 {
                    if assembling.count > 4, assembling[1] == 0x42, assembling[3] == 0x68 {
                        replies.append(assembling)
                    }
                    assembling = []
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

let v21 = UInt32(bitPattern: value) & 0x1FFFFF
let valueL = UInt8(v21 & 0x7F)
let valueM = UInt8((v21 >> 7) & 0x7F)
let valueH = UInt8((v21 >> 14) & 0x7F)
let sysex: [UInt8] = [0xF0, 0x42, 0x30 | (channel & 0x0F), 0x68, 0x43, typ, soc, sub, pid, idx, valueH, valueM, valueL, 0xF7]

let bufSize = 128
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)
check(MIDISend(outPort, destination, listPtr), "MIDISend")
print("sent: \(sysex.map { String(format: "%02X", $0) }.joined(separator: " "))")

let deadline = Date().addingTimeInterval(timeout)
while Date() < deadline {
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}

lock.lock()
let got = replies
lock.unlock()

if got.isEmpty {
    print("no reply seen within \(timeout)s")
} else {
    for r in got {
        print("reply: \(r.map { String(format: "%02X", $0) }.joined(separator: " "))")
    }
}
