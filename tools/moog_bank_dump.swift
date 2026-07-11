// Standalone All Presets Dump Request/capture, for reverse-engineering and
// restore-verification work from the terminal without going through the
// full SynthEdit GUI app — companion to moog_dump.swift (Panel Dump,
// mode 0x05/0x02) and moog_preset_dump.swift (Single Preset Dump,
// mode 0x06/0x03), for the mode 0x04/0x01 pair instead: the WHOLE bank in
// one message, same request synth_request_all_presets_dump()
// (src/synthComms.c) sends.
//
// Sends F0 04 01 00 04 F7 (mode 0x04, All Presets Dump REQUEST) and saves
// the first F0 04 01 ... reply whose mode byte is 0x01 (All Presets Dump)
// to a raw .syx file, same format the app's own Backup > Bank writes.
// Confirmed real capture size on this owner's base Voyager: 18734 bytes —
// default timeout here is longer than the other tools' (10s, not 2-3s) to
// give a message that size time to actually arrive over real DIN MIDI.
//
// usage: moog_bank_dump <destination-name-substring> <output.syx> [timeoutSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 3 else {
    err("usage: moog_bank_dump <destination-name-substring> <output.syx> [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
let outputPath = CommandLine.arguments[2]
let timeout = CommandLine.arguments.count >= 4 ? (Double(CommandLine.arguments[3]) ?? 10.0) : 10.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("moog_bank_dump" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var capturedData: [UInt8]? = nil
var packetsSeen = 0
// Same packet-reassembly reasoning as moog_dump.swift's own comment: real
// DIN MIDI over this hardware delivers a long SysEx split across many
// small packets, not one packet holding the whole message — a bank dump
// is the largest message this app ever sends/receives, so this matters
// even more here than for the other tools.
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
                        // mode byte is assembling[4]: F0 mfrId productId deviceId mode ...
                        if assembling.count > 5, assembling[1] == 0x04, assembling[2] == 0x01, assembling[4] == 0x01 {
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

let sysex: [UInt8] = [0xF0, 0x04, 0x01, 0x00, 0x04, 0xF7]

let bufSize = 1024
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, sysex.count, sysex)

check(MIDISend(outPort, destination, listPtr), "MIDISend")

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
