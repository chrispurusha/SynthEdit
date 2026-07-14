// Passive listener: prints every SysEx message received from the Kronos
// (any func, not just a specific reply), decoding func 0x43 Parameter
// Change fields (TYP/SOC/SUB/PID/IDX/value) since that's what a physical
// front-panel knob turn appears to emit unsolicited. Built to capture
// which TYP/SOC/SUB/PID/IDX each of the Kronos's "Realtime Knobs" /
// per-engine filter controls actually addresses, without needing to
// send anything ourselves.
//
// usage: kronos_monitor <source-name-substring> [durationSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 2 else {
    err("usage: kronos_monitor <source-name-substring> [durationSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
let duration = CommandLine.arguments.count >= 3 ? (Double(CommandLine.arguments[2]) ?? 30.0) : 30.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_monitor" as CFString, nil, nil, &client), "MIDIClientCreate")

let lock = NSLock()
var assembling: [UInt8] = []

func decodeAndPrint(_ msg: [UInt8]) {
    let ts = String(format: "%.3f", Date().timeIntervalSince1970)
    let hex = msg.map { String(format: "%02X", $0) }.joined(separator: " ")
    // F0 42 3<ch> 68 <func> ... F7
    guard msg.count > 4, msg[1] == 0x42, msg[3] == 0x68 else {
        print("[\(ts)] (non-Kronos or unrecognised) \(hex)")
        return
    }
    let funcId = msg[4]
    if funcId == 0x43, msg.count >= 13 {
        let typ = msg[5], soc = msg[6], sub = msg[7], pid = msg[8], idx = msg[9]
        let vH = UInt32(msg[10]), vM = UInt32(msg[11]), vL = UInt32(msg[12])
        var v21 = (vH << 14) | (vM << 7) | vL
        if v21 & 0x100000 != 0 { v21 |= 0xFFE0_0000 } // sign-extend 21-bit
        let value = Int32(bitPattern: v21)
        print("[\(ts)] Parameter Change: TYP=\(typ) SOC=\(soc) SUB=\(sub) PID=\(pid) IDX=\(idx) value=\(value)   raw: \(hex)")
    } else {
        print("[\(ts)] func=0x\(String(format: "%02X", funcId))  raw: \(hex)")
    }
}

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
                    let msg = assembling
                    assembling = []
                    lock.unlock()
                    decodeAndPrint(msg)
                    lock.lock()
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

guard let source = findEndpoint(matching: portNameSubstring, source: true) else {
    err("no MIDI source matching '\(portNameSubstring)'")
    exit(1)
}
check(MIDIPortConnectSource(inPort, source, nil), "MIDIPortConnectSource")

print("listening on source matching '\(portNameSubstring)' for \(duration)s — turn knobs on the Kronos now")

let deadline = Date().addingTimeInterval(duration)
while Date() < deadline {
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}

print("done.")
