import AppKit
import ServiceManagement

/// From an earlier build of Shenzhen KVM installed by hand: its login entry, its leftover marker and its
/// window choices. Runs once, only in the real app (not in test copies).
enum Migration {
    static var isRealApp: Bool {
        Bundle.main.bundleIdentifier == Update.bundleID && ProcessInfo.processInfo.environment["SHENZHEN_KVM_DIR"] == nil
    }

    /// Returns true when an earlier install was found (the window then explains what to do).
    static func run() -> Bool {
        guard isRealApp, !UserDefaults.standard.bool(forKey: "migrated1") else { return false }
        UserDefaults.standard.set(true, forKey: "migrated1")
        let fm = FileManager.default, home = fm.homeDirectoryForCurrentUser
        var found = false

        // 1. An earlier login entry that starts "Shenzhen KVM.app" under another name: stop it and remove it.
        let agents = home.appendingPathComponent("Library/LaunchAgents")
        for f in (try? fm.contentsOfDirectory(at: agents, includingPropertiesForKeys: nil)) ?? [] where f.pathExtension == "plist" {
            guard let d = NSDictionary(contentsOf: f) as? [String: Any], let label = d["Label"] as? String, label != Update.agentLabel,
                  let prog = (d["ProgramArguments"] as? [String])?.first, prog.hasSuffix("/Shenzhen KVM.app/Contents/MacOS/shenzhen-kvm") else { continue }
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/bin/launchctl"); p.arguments = ["bootout", "gui/\(getuid())/\(label)"]
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            try? p.run(); p.waitUntilExit()
            try? fm.removeItem(at: f)
            log("migration: removed the earlier login entry \(f.lastPathComponent)")
            found = true
        }
        if found { usleep(800_000) }   // the earlier app gives the pointer back and frees its ports

        // 2. An earlier copy of the app in ~/Applications (another bundle id): to the Trash.
        let old = home.appendingPathComponent("Applications/Shenzhen KVM.app")
        if old.standardizedFileURL != Bundle.main.bundleURL.standardizedFileURL, let b = Bundle(url: old),
           b.bundleIdentifier != Update.bundleID, b.executableURL?.lastPathComponent == "shenzhen-kvm" {
            if (try? fm.trashItem(at: old, resultingItemURL: nil)) != nil { log("migration: moved the earlier app to the Trash"); found = true }
        }

        // 3. Its folder: the AirDrop/Handoff pause it may have left on, and the window's choices.
        let oldDir = home.appendingPathComponent(".config/kvm-share")
        let marker = oldDir.appendingPathComponent("awdl-down")
        if fm.fileExists(atPath: marker.path) {
            let p = Process(); p.executableURL = URL(fileURLWithPath: "/usr/bin/sudo"); p.arguments = ["-n", "/sbin/ifconfig", "awdl0", "up"]
            p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
            try? p.run(); p.waitUntilExit()
            try? fm.removeItem(at: marker)
            found = true
        }
        if !fm.fileExists(atPath: configURL.path), let d = try? Data(contentsOf: oldDir.appendingPathComponent("config.json")),
           let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any] {
            var c = Config()
            c.apply(local: j)
            if let v = j["speed"] as? Double { c.speed = v }
            if let v = j["smooth"] as? Bool { c.smooth = v }
            if let v = j["copy_key"] as? [Int], v.count == 2 { c.copyKey = Shortcut(vk: UInt16(v[0]), flags: UInt64(v[1])) }
            if let v = j["paste_key"] as? [Int], v.count == 2 { c.pasteKey = Shortcut(vk: UInt16(v[0]), flags: UInt64(v[1])) }
            c.save()
            log("migration: kept the earlier window choices")
            found = true
        } else if fm.fileExists(atPath: oldDir.path) { found = true }
        return found
    }

    /// Starts at login (and again after a crash) through the agent inside the app.
    static func registerLoginItem() {
        guard isRealApp, Bundle.main.bundlePath.hasPrefix("/Applications/") else { return }
        let s = SMAppService.agent(plistName: Update.agentPlist)
        guard s.status != .enabled else { return }
        do { try s.register(); log("starts at login") } catch { log("could not set it to start at login: \(error)") }
    }

    static func explain(over w: NSWindow) {
        let a = NSAlert()
        a.messageText = "Shenzhen KVM has been updated"
        a.informativeText = "macOS asks again before Shenzhen KVM can use the keyboard and mouse: choose Open System Settings when asked and turn on Shenzhen KVM.\n\nThen link this Mac with your other computer again: make one of them visible and choose it on the other. The other computer needs the new Shenzhen KVM too."
        a.addButton(withTitle: "OK")
        a.beginSheetModal(for: w, completionHandler: nil)
    }
}
