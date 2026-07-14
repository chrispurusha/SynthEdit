// Minimal Z1 identity emulator + traffic logger, for testing SynthEdit's Z1
// support without real hardware connected — built 2026-07-14 to test the new
// Korg-style "Restore Edit Buffer" (loading a Program Data Dump .syx file into the
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

// Inverse of decode7to8 above — packs 8-bit bytes into a MIDI-safe 7-bit
// stream (groups of up to 7 data bytes, each preceded by one byte holding
// their stripped MSBs), same grouping synth_decode_korg_name()'s own
// decode_7to8() (synthComms.c) expects on the way back in. Added 2026-07-14
// to synthesize a fake Program Data Dump reply — needed to test the new
// "Save Patch by Number to File" (Z1) round trip end to end, since without
// this the emulator could only ever log requests, never answer them.
func encode8to7(_ data: [UInt8]) -> [UInt8] {
    var out: [UInt8] = []
    var i = 0
    while i < data.count {
        let chunk = data[i..<min(i + 7, data.count)]
        var msbs: UInt8 = 0
        for (j, b) in chunk.enumerated() {
            if (b & 0x80) != 0 { msbs |= (1 << j) }
        }
        out.append(msbs)
        for b in chunk { out.append(b & 0x7F) }
        i += 7
    }
    return out
}

// Builds a synthetic PROGRAM DATA DUMP reply (func 0x4C) for a Program Data
// Dump Request (func 0x1C) — F0 42 3<ch> 46 4C <ub> <pp> 00 <7-bit payload> F7,
// per handle_prog_dump()/korg_decode_prog_dump()'s own comments (synthComms.c).
// Payload here is just a printable name (progNameLen=16, per layouts/z1.txt)
// padded to 16 chars, plus a little filler — real Z1 payloads are much
// bigger, but nothing downstream of synth_backup_capture_dump() needs more
// than the name for this test.
func sendProgramDumpReply(bank: UInt8, prog: UInt8) {
    let label = "A\(String(format: "%03d", Int(prog) + 1))"
    var name = "Test \(label)"
    if name.count > 16 { name = String(name.prefix(16)) }
    while name.count < 16 { name += " " }
    var payload = Array(name.utf8)
    payload += [UInt8](repeating: 0, count: 8) // small filler, unused by the test
    let encoded = encode8to7(payload)
    var msg: [UInt8] = [0xF0, 0x42, 0x30, 0x46, 0x4C, bank & 0x01, prog & 0x7F, 0x00]
    msg += encoded
    msg.append(0xF7)
    send(msg)
    print("-> Program Data Dump reply (bank=\(bank == 0 ? "A" : "B") prog=\(Int(prog) + 1) name=\"\(name)\"): \(msg.map { String(format: "%02X", $0) }.joined(separator: " "))")
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
    } else if funcId == 0x1C, msg.count >= 9 {
        // Program Data Dump Request: F0 42 3g 46 1C ub pp 00 F7 — answer
        // with a synthetic reply (see sendProgramDumpReply() above) so the
        // app's own capture-and-save path can be exercised end to end,
        // not just the outgoing request.
        let bank = msg[5] & 0x01
        let prog = msg[6]
        print("<- Program Data Dump Request (bank=\(bank == 0 ? "A" : "B") prog=\(Int(prog) + 1))   raw: \(hex)")
        sendProgramDumpReply(bank: bank, prog: prog)
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
