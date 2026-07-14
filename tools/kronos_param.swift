// Standalone Kronos Parameter Change (integer) sender, for reverse-
// engineering work from the terminal — companion to kronos_dump.swift.
//
// Sends func 0x43 per KRONOS_MIDI_SysEx.txt:
//   F0 42 3g 68 43 <TYP> <SOC> <SUB> <PID> <IDX> <valueH> <valueM> <valueL> F7
// value is a 21-bit 2's complement integer (*4 in the doc); for a small
// positive value (0-99 range params like Filter Cutoff) valueH=valueM=0.
//
// usage: kronos_param <destination-name-substring> <TYP> <SOC> <SUB> <PID> <IDX> <value> [channel0based]
//   e.g.: kronos_param kronos 11 20 0 4 0 20 0
//         (EXi1 slot, comp 20 = Analog 4-Pole Filter, PID 4 = Filter A Cutoff, value 20)
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

guard CommandLine.arguments.count >= 8 else {
    err("usage: kronos_param <destination-name-substring> <TYP> <SOC> <SUB> <PID> <IDX> <value> [channel0based]")
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

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("kronos_param" as CFString, nil, nil, &client), "MIDIClientCreate")

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

// 21-bit 2's complement, split into three 7-bit groups (*4 in the doc):
// valueH: bit14-20, valueM: bit7-13, valueL: bit0-6.
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
print("sent Parameter Change: TYP=\(typ) SOC=\(soc) SUB=\(sub) PID=\(pid) IDX=\(idx) value=\(value) (bytes: \(sysex.map { String(format: "%02X", $0) }.joined(separator: " ")))")
