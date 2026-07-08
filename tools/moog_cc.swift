// Sends a plain 3-byte MIDI CC message — for testing whether a guessed CC
// assignment (voyager.txt) actually does anything on real hardware, and/or
// correlating it against a before/after Panel Dump diff (tools/syx_diff.py)
// to locate its real dump bit position, the same way moog_dump/moog_send
// were used to reverse-engineer Filter A/B Pole Select.
//
// usage: moog_cc <destination-name-substring> <channel 1-indexed> <cc> <value>
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

guard CommandLine.arguments.count == 5,
      let channel1 = UInt8(CommandLine.arguments[2]),
      let cc = UInt8(CommandLine.arguments[3]),
      let value = UInt8(CommandLine.arguments[4]) else {
    err("usage: moog_cc <destination-name-substring> <channel 1-indexed> <cc> <value>")
    exit(1)
}
let portNameSubstring = CommandLine.arguments[1].lowercased()
let channelIndex = (channel1 - 1) & 0x0F

var client = MIDIClientRef()
check(MIDIClientCreate("moog_cc" as CFString, nil, nil, &client), "MIDIClientCreate")

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

let bytes: [UInt8] = [0xB0 | channelIndex, cc, value]
var packetList = MIDIPacketList()
let packetPtr = MIDIPacketListInit(&packetList)
_ = MIDIPacketListAdd(&packetList, 1024, packetPtr, 0, bytes.count, bytes)

check(MIDISend(outPort, destination, &packetList), "MIDISend")
print("sent CC\(cc)=\(value) on channel \(channel1)")
