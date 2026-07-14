// Minimal Z1 identity emulator + traffic logger, for testing SynthEdit's Z1
// support without real hardware connected — built 2026-07-14 to test the new
// Korg-style "Restore Panel" (loading a Program Data Dump .syx file into the
// live edit buffer via individual Parameter Change messages, since Z1 has no
// single "load this dump" SysEx the way a Moog-style device does).
//
// Listens on IAC Driver Bus 1 (a macOS-local loopback bus — the real app's
// connect flow broadcasts an Identity Request to every visible destination
// and listens on every visible source, so this just needs to be visible on
// that same bus, no app-side config change needed): answers a Universal
// Identity Request with a Korg Z1-shaped reply (mfrId=0x42 familyId=0x46
// memberId=0x01, matching layouts/z1.txt), then logs and decodes every
// subsequent SysEx byte-for-byte — Parameter Change (func 0x41) decoded into
// group/param/value (and, for the two known wireSigned params 163/164,
// the sign-extended real value too), everything else just hex-dumped with
// its func byte called out.
//
// usage: z1_emulator [durationSeconds]
import CoreMIDI
import Foundation

func err(_ s: String) {
    FileHandle.standardError.write((s + "\n").data(using: .utf8)!)
}

setbuf(stdout, nil) // unbuffered — this is normally run with stdout redirected to a file for live tailing, which would otherwise fully buffer and only flush at exit

let duration = CommandLine.arguments.count >= 2 ? (Double(CommandLine.arguments[1]) ?? 120.0) : 120.0

func check(_ status: OSStatus, _ what: String) {
    if status != 0 {
        err("\(what) failed: status=\(status)")
        exit(1)
    }
}

var client = MIDIClientRef()
check(MIDIClientCreate("z1_emulator" as CFString, nil, nil, &client), "MIDIClientCreate")

var outPort = MIDIPortRef()
check(MIDIOutputPortCreate(client, "out" as CFString, &outPort), "MIDIOutputPortCreate")

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

guard let iacDest = findEndpoint(matching: "iac", source: false) else {
    err("no IAC destination found — enable 'IAC Driver Bus 1' in Audio MIDI Setup")
    exit(1)
}
guard let iacSource = findEndpoint(matching: "iac", source: true) else {
    err("no IAC source found — enable 'IAC Driver Bus 1' in Audio MIDI Setup")
    exit(1)
}

func send(_ bytes: [UInt8]) {
    let bufSize = 256
    let rawList = UnsafeMutableRawPointer.allocate(byteCount: bufSize, alignment: 4)
    defer { rawList.deallocate() }
    let listPtr = rawList.bindMemory(to: MIDIPacketList.self, capacity: 1)
    let packetPtr = MIDIPacketListInit(listPtr)
    _ = MIDIPacketListAdd(listPtr, bufSize, packetPtr, 0, bytes.count, bytes)
    _ = MIDISend(outPort, iacDest, listPtr)
}

func decode7to8(_ midi: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    var i = 0
    while i + 7 < midi.count {
        let msbs = midi[i]
        for j in 0..<7 {
            out.append(midi[i + 1 + j] | (((msbs >> j) & 1) << 7))
        }
        i += 8
    }
    return out
}

func decodeSigned14(_ v: Int) -> Int {
    return v >= 8192 ? v - 16384 : v
}

func handleSysex(_ msg: [UInt8]) {
    let hex = msg.map { String(format: "%02X", $0) }.joined(separator: " ")

    guard msg.count > 4 else {
        print("(short) \(hex)")
        return
    }

    // Universal Identity Request: F0 7E <ch> 06 01 F7
    if msg[1] == 0x7E, msg.count >= 6, msg[3] == 0x06, msg[4] == 0x01 {
        print("<- Identity Request (ch=0x\(String(format: "%02X", msg[2])))")
        let reply: [UInt8] = [0xF0, 0x7E, 0x00, 0x06, 0x02, 0x42, 0x46, 0x00, 0x01, 0x00, 0xF7]
        send(reply)
        print("-> Identity Reply (Z1: mfr=0x42 fam=0x46 mem=0x01): \(reply.map { String(format: "%02X", $0) }.joined(separator: " "))")
        return
    }

    // Korg-shaped header: F0 42 3<ch> 46 <func> ...
    guard msg[1] == 0x42, (msg[2] & 0xF0) == 0x30, msg[3] == 0x46 else {
        print("(non-Z1 or unrecognised) \(hex)")
        return
    }
    let funcId = msg[4]

    if funcId == 0x41, msg.count >= 11 {
        // F0 42 3g 46 41 0mm pp pp vv vv F7
        let group = msg[5] & 0x0F
        let paramId = Int(msg[6]) | (Int(msg[7] & 0x7F) << 7)
        let value = Int(msg[8]) | (Int(msg[9] & 0x7F) << 7)
        var extra = ""
        if paramId == 163 || paramId == 164 {
            let signed14 = decodeSigned14(value)
            let label = paramId == 163 ? "PB Int+" : "PB Int-"
            extra = "  [\(label): raw14=\(value) -> signed=\(signed14) -> display=\(signed14 + 60)]"
        }
        print("<- Parameter Change: group=\(group) param=\(paramId) value=\(value)\(extra)   raw: \(hex)")
    } else {
        print("<- func=0x\(String(format: "%02X", funcId)) len=\(msg.count)   raw: \(hex)")
    }
}

let lock = NSLock()
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
                // System Real-Time — spec-legal mid-SysEx, must not be appended
            } else if !assembling.isEmpty {
                assembling.append(b)
                if b == 0xF7 {
                    let msg = assembling
                    assembling = []
                    lock.unlock()
                    handleSysex(msg)
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
check(MIDIPortConnectSource(inPort, iacSource, nil), "MIDIPortConnectSource")

print("z1_emulator listening on IAC for \(duration)s — waiting for the app's Identity Request...")

let deadline = Date().addingTimeInterval(duration)
while Date() < deadline {
    RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.05))
}
print("done.")
