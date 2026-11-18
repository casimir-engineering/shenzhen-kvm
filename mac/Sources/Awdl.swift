import Foundation

/// Keeps awdl0 (AirDrop/Handoff radio link) down while the mouse is shared, so the Wi-Fi radio stays
/// on the network's channel. Uses the sudoers rule from install-smooth.sh; without it, does nothing.
final class Awdl {
    private let q = DispatchQueue(label: "awdl", qos: .userInitiated)
    private let marker = configDir.appendingPathComponent("awdl-down")
    private(set) var installed = false
    private var weDowned = false
    private var lastCheck = 0.0, lastInstallCheck = -100.0
    var onChange: (() -> Void)?

    init() {
        q.async {
            self.refreshInstalled()
            // A previous run left it down (crash, restart): bring it back.
            if FileManager.default.fileExists(atPath: self.marker.path) { self.run("up"); try? FileManager.default.removeItem(at: self.marker) }
        }
    }

    static func isUp() -> Bool {
        var list: UnsafeMutablePointer<ifaddrs>?
        guard getifaddrs(&list) == 0 else { return true }
        defer { freeifaddrs(list) }
        var p = list
        while let a = p {
            if String(cString: a.pointee.ifa_name) == "awdl0" { return a.pointee.ifa_flags & UInt32(IFF_UP) != 0 }
            p = a.pointee.ifa_next
        }
        return false
    }

    @discardableResult
    private func run(_ arg: String) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/sudo")
        p.arguments = ["-n", "/sbin/ifconfig", "awdl0", arg]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    private func refreshInstalled() {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/sudo")
        p.arguments = ["-n", "-l", "/sbin/ifconfig", "awdl0", "down"]
        p.standardOutput = FileHandle.nullDevice; p.standardError = FileHandle.nullDevice
        var ok = false
        if (try? p.run()) != nil { p.waitUntilExit(); ok = p.terminationStatus == 0 }
        if ok != installed { installed = ok; log("smooth pointer rule \(ok ? "found" : "not installed")"); DispatchQueue.main.async { self.onChange?() } }
    }

    /// Call on every session change (immediately) and from a timer; `want` = a session is active and the option is on.
    func sync(want: Bool, force: Bool = false) {
        q.async {
            let now = mono()
            if !self.installed {
                if now - self.lastInstallCheck > 10 { self.lastInstallCheck = now; self.refreshInstalled() }
                if !self.installed { return }
            }
            if !force && now - self.lastCheck < 0.1 { return }
            self.lastCheck = now
            if want {
                if Awdl.isUp() {
                    if self.run("down") {
                        if !self.weDowned { log("AirDrop/Handoff link paused (awdl0 down)") }
                        self.weDowned = true
                        try? Data().write(to: self.marker)
                    }
                }
            } else if self.weDowned {
                if self.run("up") { log("AirDrop/Handoff link back on (awdl0 up)") }
                self.weDowned = false
                try? FileManager.default.removeItem(at: self.marker)
            }
        }
    }

    /// Blocking: bring awdl0 back before exit.
    func restoreNow() {
        q.sync {
            if self.weDowned || FileManager.default.fileExists(atPath: self.marker.path) {
                self.run("up"); self.weDowned = false; try? FileManager.default.removeItem(at: self.marker)
            }
        }
    }
}
