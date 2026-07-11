// Standalone Single Preset Dump Request/capture, for reverse-engineering
// and restore-testing work from the terminal without going through the
// full SynthEdit GUI app — companion to moog_dump.swift (which only ever
// requests the Panel Dump / live edit buffer, mode 0x05/0x02), for the
// mode 0x06/0x03 pair instead: a SPECIFIC stored preset, addressed by
// number, same request synth_request_single_preset_dump() (src/synthComms.c)
// sends.
//
// Sends F0 04 01 00 06 <presetNumber-1> F7 (mode 0x06, Single Preset Dump
// REQUEST — 0-based preset number on the wire, per the same header this
// app's own request builds) and saves the first F0 04 01 ... reply whose
// mode byte is 0x03 (Single Preset Dump) to a raw .syx file, same format
// the app's own Backup > Patch by Number writes.
//
// usage: moog_preset_dump <destination-name-substring> <presetNumber 1-128> <output.syx> [timeoutSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 4 else {
    err("usage: moog_preset_dump <destination-name-substring> <presetNumber 1-128> <output.syx> [timeoutSeconds]")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
guard let presetNumber = Int(CommandLine.arguments[2]), presetNumber >= 1, presetNumber <= 128 else {
    err("presetNumber must be 1-128")
    exit(1)
}
let outputPath = CommandLine.arguments[3]
let timeout = CommandLine.arguments.count >= 5 ? (Double(CommandLine.arguments[4]) ?? 2.0) : 2.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("moog_preset_dump" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

let lock = NSLock()
var capturedData: [UInt8]? = nil
var packetsSeen = 0
// Same packet-reassembly reasoning as moog_dump.swift's own comment: real
// DIN MIDI over this hardware delivers a long SysEx split across many
// small packets, not one packet holding the whole message.
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
                        if assembling.count > 5, assembling[1] == 0x04, assembling[2] == 0x01, assembling[4] == 0x03 {
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

let sysex: [UInt8] = [0xF0, 0x04, 0x01, 0x00, 0x06, UInt8(presetNumber - 1), 0xF7]

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
print("wrote \(data.count) bytes to \(outputPath) (preset \(presetNumber))")
