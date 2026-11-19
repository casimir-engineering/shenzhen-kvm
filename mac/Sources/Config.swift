import Foundation
import CryptoKit

enum Edge: String, CaseIterable {
    case left, right, top, bottom
    var wire: UInt8 {
        switch self { case .left: return 0; case .right: return 1; case .top: return 2; case .bottom: return 3 }
    }
    init?(wire: UInt8) {
        switch wire { case 0: self = .left; case 1: self = .right; case 2: self = .top; case 3: self = .bottom; default: return nil }
    }
    var opposite: Edge {
        switch self { case .left: return .right; case .right: return .left; case .top: return .bottom; case .bottom: return .top }
    }
}

/// The app's folder: ~/Library/Application Support/Shenzhen KVM (SHENZHEN_KVM_DIR for test copies).
let configDir: URL = {
    if let d = ProcessInfo.processInfo.environment["SHENZHEN_KVM_DIR"], !d.isEmpty { return URL(fileURLWithPath: d) }
    return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/Shenzhen KVM")
}()
let configURL = configDir.appendingPathComponent("config.json")
let linkURL = configDir.appendingPathComponent("link.json")
let idURL = configDir.appendingPathComponent("id")

/// Ports: input (UDP), clipboard (TCP) = input + 1, finding and linking = input + 2. SHENZHEN_KVM_PORT_BASE moves all three (tests).
let portBase: UInt16 = {
    if let s = ProcessInfo.processInfo.environment["SHENZHEN_KVM_PORT_BASE"], let v = UInt16(s), v > 0, v < 65530 { return v }
    return 24800
}()
let discPort: UInt16 = portBase + 2

/// Tests: no event tap, nothing injected, no Accessibility prompt (SHENZHEN_KVM_NO_INPUT=1).
let inputDisabled = ProcessInfo.processInfo.environment["SHENZHEN_KVM_NO_INPUT"] == "1"

func hex(_ b: some Sequence<UInt8>) -> String { b.map { String(format: "%02x", $0) }.joined() }
func unhex(_ s: String) -> [UInt8]? {
    let t = Array(s.utf8); guard t.count % 2 == 0 else { return nil }
    var out = [UInt8](); out.reserveCapacity(t.count / 2)
    var i = 0
    while i < t.count {
        guard let v = UInt8(String(bytes: t[i..<i + 2], encoding: .ascii) ?? "", radix: 16) else { return nil }
        out.append(v); i += 2
    }
    return out
}

/// This computer's id (16 random bytes, made once).
let myID: [UInt8] = {
    if let s = try? String(contentsOf: idURL, encoding: .utf8), let b = unhex(s.trimmingCharacters(in: .whitespacesAndNewlines)), b.count == 16 { return b }
    var b = [UInt8](repeating: 0, count: 16); _ = SecRandomCopyBytes(kSecRandomDefault, 16, &b)
    try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)
    try? (hex(b) + "\n").write(to: idURL, atomically: true, encoding: .utf8)
    return b
}()
let myIDHex = hex(myID)

/// The linked computer (PROTOCOL.md Identity and link). Kept in link.json, mode 0600.
struct Link {
    var peerID: [UInt8]
    var peerName: String
    var key: [UInt8]
    var linkID: [UInt8]
    var peerDP: UInt16
    var peerIDHex: String { hex(peerID) }

    static func load() -> Link? {
        guard let d = try? Data(contentsOf: linkURL), let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
              let pid = (j["peer_id"] as? String).flatMap(unhex), pid.count == 16,
              let k = (j["key"] as? String).flatMap(unhex), k.count == 32,
              let lid = (j["link_id"] as? String).flatMap(unhex), lid.count == 8 else { return nil }
        return Link(peerID: pid, peerName: j["peer_name"] as? String ?? "the other computer", key: k, linkID: lid,
                    peerDP: UInt16(j["peer_dp"] as? Int ?? 24800))
    }
    func save() -> Bool {
        let j: [String: Any] = ["peer_id": hex(peerID), "peer_name": peerName, "key": hex(key), "link_id": hex(linkID), "peer_dp": Int(peerDP)]
        guard let d = try? JSONSerialization.data(withJSONObject: j, options: [.sortedKeys]) else { return false }
        try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)
        let tmp = linkURL.appendingPathExtension("new")
        guard FileManager.default.createFile(atPath: tmp.path, contents: d, attributes: [.posixPermissions: 0o600]) else { return false }
        return rename(tmp.path, linkURL.path) == 0
    }
    static func forget() { try? FileManager.default.removeItem(at: linkURL) }
}

struct Config {
    var edge: Edge = .top        // the side of this Mac's screens where the other computer's screen is
    var speed = 1.0
    var push = 20.0              // pixels pushed against the edge before crossing
    var paused = false
    var smooth = true            // pause AirDrop/Handoff (awdl0) while sharing
    var macDrivesPeer = true
    var peerDrivesMac = true
    var clipboard = true
    var usualShortcuts = true    // shared clipboard with the ordinary copy/paste
    var bigFiles = true
    var fileLimitMB = 100
    var commandSuper = false     // on Linux, the Mac's Command acts as Super (else Control)
    var changed: Int64 = 0       // ms since 1970 of the last change to the shared settings
    var copyKey = Shortcut(vk: 8, flags: 0x120000)    // Command+Shift+C
    var pasteKey = Shortcut(vk: 9, flags: 0x120000)   // Command+Shift+V

    static func load() -> Config {
        var c = Config()
        guard let d = try? Data(contentsOf: configURL),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] else { return c }
        c.apply(local: j)
        if let v = j["speed"] as? Double { c.speed = v }
        if let v = j["push"] as? Double { c.push = v }
        if let v = j["smooth"] as? Bool { c.smooth = v }
        if let v = j["changed"] as? Int64 { c.changed = v } else if let v = j["changed"] as? Int { c.changed = Int64(v) }
        if let v = j["copy_key"] as? [Int], v.count == 2 { c.copyKey = Shortcut(vk: UInt16(v[0]), flags: UInt64(v[1])) }
        if let v = j["paste_key"] as? [Int], v.count == 2 { c.pasteKey = Shortcut(vk: UInt16(v[0]), flags: UInt64(v[1])) }
        return c
    }

    mutating func apply(local j: [String: Any]) {
        if let v = j["edge"] as? String, let e = Edge(rawValue: v) { edge = e }
        if let v = j["paused"] as? Bool { paused = v }
        if let v = j["mac_drives_peer"] as? Bool { macDrivesPeer = v }
        if let v = j["peer_drives_mac"] as? Bool { peerDrivesMac = v }
        if let v = j["clipboard"] as? Bool { clipboard = v }
        if let v = j["usual_shortcuts"] as? Bool { usualShortcuts = v }
        if let v = j["big_files"] as? Bool { bigFiles = v }
        if let v = j["file_limit_mb"] as? Int, v > 0 { fileLimitMB = v }
        if let v = j["command_as"] as? String { commandSuper = v == "super" }
    }

    func save() {
        let j: [String: Any] = ["edge": edge.rawValue, "speed": speed, "push": push, "paused": paused, "smooth": smooth,
                                "mac_drives_peer": macDrivesPeer, "peer_drives_mac": peerDrivesMac,
                                "clipboard": clipboard, "usual_shortcuts": usualShortcuts, "big_files": bigFiles,
                                "file_limit_mb": fileLimitMB, "command_as": commandSuper ? "super" : "control", "changed": changed,
                                "copy_key": [Int(copyKey.vk), Int(copyKey.flags)], "paste_key": [Int(pasteKey.vk), Int(pasteKey.flags)]]
        try? FileManager.default.createDirectory(at: configDir, withIntermediateDirectories: true)
        if let d = try? JSONSerialization.data(withJSONObject: j, options: [.prettyPrinted, .sortedKeys]) {
            try? d.write(to: configURL)
        }
    }

    /// SETTINGS (PROTOCOL.md), from this Mac's side.
    var wireJSON: [UInt8] {
        let j: [String: Any] = ["sharing": !paused, "i_drive_you": macDrivesPeer, "you_drive_me": peerDrivesMac,
                                "clipboard": clipboard, "usual_shortcuts": usualShortcuts, "big_files": bigFiles,
                                "file_limit_mb": fileLimitMB, "my_edge": edge.rawValue, "command_as": commandSuper ? "super" : "control",
                                "changed": changed]
        return Array(try! JSONSerialization.data(withJSONObject: j, options: [.sortedKeys]))
    }

    /// The shared values (not when they changed), to tell a real change from a repeat.
    var sharedValues: String { "\(paused)\(macDrivesPeer)\(peerDrivesMac)\(clipboard)\(usualShortcuts)\(bigFiles)\(fileLimitMB)\(edge)\(commandSuper)" }
    enum Adoption { case older, changed, same }
    /// SETTINGS from the peer: adopted when newer.
    mutating func adopt(wire j: [String: Any]) -> Adoption {
        let ch = (j["changed"] as? Int64) ?? Int64(j["changed"] as? Int ?? 0)
        if ch != 0 && ch <= changed { return .older }
        let before = sharedValues
        if let v = j["sharing"] as? Bool { paused = !v }
        if let v = j["i_drive_you"] as? Bool { peerDrivesMac = v }
        if let v = j["you_drive_me"] as? Bool { macDrivesPeer = v }
        if let v = j["clipboard"] as? Bool { clipboard = v }
        if let v = j["usual_shortcuts"] as? Bool { usualShortcuts = v }
        if let v = j["big_files"] as? Bool { bigFiles = v }
        if let v = j["file_limit_mb"] as? Int, v > 0 { fileLimitMB = v }
        if let v = j["my_edge"] as? String, let e = Edge(rawValue: v) { edge = e.opposite }
        if let v = j["command_as"] as? String { commandSuper = v == "super" }
        changed = ch != 0 ? ch : Int64(Date().timeIntervalSince1970 * 1000)
        return sharedValues == before ? .same : .changed
    }
}

/// A key with modifiers (CGEventFlags bits for Command, Shift, Option, Control).
struct Shortcut: Equatable {
    var vk: UInt16
    var flags: UInt64
    static let mask: UInt64 = 0x100000 | 0x20000 | 0x80000 | 0x40000
    func matches(vk v: UInt16, flags f: UInt64) -> Bool { v == vk && (f & Shortcut.mask) == (flags & Shortcut.mask) }
    var title: String {
        var s = ""
        if flags & 0x40000 != 0 { s += "⌃" }
        if flags & 0x80000 != 0 { s += "⌥" }
        if flags & 0x20000 != 0 { s += "⇧" }
        if flags & 0x100000 != 0 { s += "⌘" }
        if let h = vkToHid[vk] {
            if h >= 0x04 && h <= 0x1D { s += String(UnicodeScalar(UInt8(65 + h - 4))) }
            else if h >= 0x1E && h <= 0x27 { s += h == 0x27 ? "0" : String(h - 0x1D) }
            else { s += "key \(vk)" }
        } else { s += "key \(vk)" }
        return s
    }
}

let logURL: URL = {
    if let d = ProcessInfo.processInfo.environment["SHENZHEN_KVM_DIR"], !d.isEmpty { return URL(fileURLWithPath: d).appendingPathComponent("log.txt") }
    return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/Shenzhen KVM.log")
}()
private let logQ = DispatchQueue(label: "log")
func log(_ s: String) {
    let line = "\(ISO8601DateFormatter().string(from: Date())) \(s)\n"
    logQ.async {
        if let h = try? FileHandle(forWritingTo: logURL) {
            h.seekToEndOfFile(); h.write(line.data(using: .utf8)!); try? h.close()
        } else {
            try? line.data(using: .utf8)!.write(to: logURL)
        }
    }
}
