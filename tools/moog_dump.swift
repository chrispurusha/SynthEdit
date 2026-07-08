// Standalone Moog Panel Dump Request/capture, for reverse-engineering work
// from the terminal without going through the full SynthEdit GUI app.
//
// Sends the same stateRequestSysEx voyager.txt declares (F0 04 01 00 05 F7 —
// "Panel Dump REQUEST", mode 0x05) and saves the first F0 04 01 ... reply
// (mfrId=0x04, productId=0x01 — matches is_moog_sysex() in src/synthComms.c)
// to a raw .syx file, same format the app's own Backup feature writes.
//
// usage: moog_dump <destination-name-substring> <output.syx> [timeoutSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 3 else {
    err("usage: moog_dump <destination-name-substring> <output.syx> [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
let outputPath = CommandLine.arguments[2]
let timeout = CommandLine.arguments.count >= 4 ? (Double(CommandLine.arguments[3]) ?? 2.0) : 2.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("moog_dump" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var capturedData: [UInt8]? = nil
var packetsSeen = 0
// Real DIN MIDI arrives as a slow serial stream — CoreMIDI/the interface
// often hands it to the read callback split across many small packets
// (observed: 3 bytes at a time) rather than one packet holding the whole
// SysEx. Assemble across calls, tracking start (0xF0) through end (0xF7).
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
                } else if !assembling.isEmpty {
                    assembling.append(b)
                    if b == 0xF7 {
                        if assembling.count > 3, assembling[1] == 0x04, assembling[2] == 0x01 {
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

let sysex: [UInt8] = [0xF0, 0x04, 0x01, 0x00, 0x05, 0xF7]

let bufSize = 1024
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)

check(MIDISend(outPort, destination, listPtr), "MIDISend")

// CoreMIDI's read callback is delivered via this thread's run loop — a plain
// Thread.sleep() poll loop never pumps it, so no callback ever fires even
// when MIDI data physically arrives. Must actually run the run loop.
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
