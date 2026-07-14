// Sends a Mode Request (func 0x12) and prints the Mode Data reply (func
// 0x42) per KRONOS_MIDI_SysEx.txt: F0 42 3g 68 42 0000mmmm ... F7, where
// mmmm (*5) is 0=COMBINATION 2=PROGRAM 4=SEQUENCER 6=SAMPLING 7=GLOBAL
// 8=DISK 9=SET LIST.
//
// usage: kronos_mode <destination-name-substring> [channel0based] [timeoutSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 2 else {
    err("usage: kronos_mode <destination-name-substring> [channel0based] [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
let channel = CommandLine.arguments.count >= 3 ? (UInt8(CommandLine.arguments[2]) ?? 0) : 0
let timeout = CommandLine.arguments.count >= 4 ? (Double(CommandLine.arguments[3]) ?? 2.0) : 2.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_mode" as CFString, nil, nil, &client), "MIDIClientCreate")
var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var captured: [UInt8]? = nil
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
        if captured == nil {
            for b in bytes {
                if b == 0xF0 {
                    assembling = [b]
                } else if !assembling.isEmpty {
                    assembling.append(b)
                    if b == 0xF7 {
                        if assembling.count > 5, assembling[1] == 0x42, assembling[3] == 0x68, assembling[4] == 0x42 {
                            captured = assembling
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

let sysex: [UInt8] = [0xF0, 0x42, 0x30 | (channel & 0x0F), 0x68, 0x12, 0xF7]
let bufSize = 64
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)
check(MIDISend(outPort, destination, listPtr), "MIDISend")

let deadline = Date().addingTimeInterval(timeout)
while Date() < deadline {
    lock.lock()
    let done = captured != nil
    lock.unlock()
    if done { break }
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}

lock.lock()
let result = captured
lock.unlock()

guard let data = result else {
    err("timed out waiting for Mode Data reply")
    exit(2)
}
print("raw: \(data.map { String(format: "%02X", $0) }.joined(separator: " "))")
let mode = data[5] & 0x0F
let names = ["COMBINATION", "?", "PROGRAM", "?", "SEQUENCER", "?", "SAMPLING", "GLOBAL", "DISK", "SET LIST"]
print("mode = \(mode) (\(Int(mode) < names.count ? names[Int(mode)] : "?"))")
