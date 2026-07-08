// Sends a raw .syx file's bytes verbatim over CoreMIDI — companion to
// moog_dump.swift, for testing whether the Voyager accepts an incoming
// Panel Dump message (the same shape moog_dump captures) as a "load this
// state" command, rather than only ever emitting that shape itself.
//
// usage: moog_send <destination-name-substring> <input.syx>
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

guard CommandLine.arguments.count == 3 else {
    err("usage: moog_send <destination-name-substring> <input.syx>")
    exit(1)
}

let portNameSubstring = CommandLine.arguments[1].lowercased()
let inputPath = CommandLine.arguments[2]

guard let fileData = FileManager.default.contents(atPath: inputPath) else {
    err("couldn't read \(inputPath)")
    exit(1)
}
let bytes = [UInt8](fileData)

guard bytes.first == 0xF0, bytes.last == 0xF7 else {
    err("\(inputPath) doesn't look like a raw F0...F7 SysEx capture (\(bytes.count) bytes)")
    exit(1)
}

var client = MIDIClientRef()
check(MIDIClientCreate("moog_send" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

func findDestination(matching substring: String) -> MIDIEndpointRef? {
    let count = MIDIGetNumberOfDestinations()
    for i in 0..<count {
        let ep = MIDIGetDestination(i)
        var cfName: Unmanaged<CFString>?
        MIDIObjectGetStringProperty(ep, kMIDIPropertyDisplayName, &cfName)
        if let name = cfName?.takeRetainedValue() as String?, name.lowercased().contains(substring) {
            return ep
        }
    }
    return nil
}

guard let destination = findDestination(matching: portNameSubstring) else {
    err("no MIDI destination matching '\(portNameSubstring)'")
    exit(1)
}

let bufSize = max(1024, bytes.count + 64)
let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
defer { rawList.deallocate() }
let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
let packetPtr = MIDIPacketListInit(listPtr)
_ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, bytes.count, bytes)

check(MIDISend(outPort, destination, listPtr), "MIDISend")
print("sent \(bytes.count) bytes from \(inputPath)")
