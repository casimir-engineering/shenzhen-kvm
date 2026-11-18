import AppKit
import Security
import CryptoKit
import ServiceManagement

// The self-updater and the disk-image installer, as in ShenzhenQR (itself ShenzhenPDF's updater, in Swift).
// The lead's code calls:
//   updaterCommandLine(CommandLine.arguments)   first thing in main.swift
//   Installer.offerMove() / Installer.ejectInstallerDisk() / Updater.shared.start()   in applicationDidFinishLaunching
//   addUpdaterMenuItems(to:)   for the app menu

/// The app's bundle.
let updaterAppBundle = Bundle.main

// MARK: - Command line

/// Handles `--post-update <staged.app> <target.app> <pid>` and `--verify-update <dmg> <version>`, and exits when it
/// did. Does nothing for any other arguments.
func updaterCommandLine(_ args: [String]) {
    if args.count == 5, args[1] == "--post-update", let pid = pid_t(args[4]) {
        Update.postUpdate(staged: URL(fileURLWithPath: args[2]), target: URL(fileURLWithPath: args[3]), pid: pid)
    }
    if args.count == 4, args[1] == "--verify-update" {
        // Test: runs the updater's checks on a local disk image, as if it were release `args[3]`.
        let dmg = URL(fileURLWithPath: args[2])
        let size = (try? FileManager.default.attributesOfItem(atPath: dmg.path)[.size] as? Int) ?? 0
        let r = Update.Release(tag: "v" + args[3], version: args[3], url: dmg, size: size, digest: nil, notes: "")
        let staging = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
        try? FileManager.default.createDirectory(at: staging, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: staging) }
        do { let staged = try Update.prepare(dmg: dmg, release: r, staging: staging); print("passes: \(staged.lastPathComponent)") }
        catch { print("refused: \(error)") }
        exit(0)
    }
}

/// Adds "Check for Updates…" and "Automatically Check for Updates" to an app menu.
func addUpdaterMenuItems(to menu: NSMenu) {
    menu.addItem(withTitle: "Check for Updates…", action: #selector(Updater.checkNow), keyEquivalent: "").target = Updater.shared
    let auto = menu.addItem(withTitle: "Automatically Check for Updates", action: #selector(Updater.toggleAutomatic(_:)), keyEquivalent: "")
    auto.target = Updater.shared
    auto.state = Update.autoUpdate ? .on : .off
}

// MARK: - Installing from the disk image

enum Installer {
    static let appName = "Shenzhen KVM.app"

    /// Mounted Shenzhen KVM disk images: the app and an Applications link at the root.
    static func installerVolumes() -> [URL] {
        let fm = FileManager.default
        return (fm.mountedVolumeURLs(includingResourceValuesForKeys: nil, options: []) ?? []).filter {
            $0.path.hasPrefix("/Volumes/")
                && fm.fileExists(atPath: $0.appendingPathComponent(appName).path)
                && (try? fm.destinationOfSymbolicLink(atPath: $0.appendingPathComponent("Applications").path)) == "/Applications"
        }
    }

    static func runningVolume() -> URL? {
        let bundle = Bundle.main.bundleURL.standardizedFileURL.path
        return installerVolumes().first { bundle.hasPrefix($0.standardizedFileURL.path + "/") }
    }

    /// Opened straight from the disk image: offer to copy it to Applications, open that copy and quit.
    /// Returns true when this instance is quitting.
    static func offerMove() -> Bool {
        guard runningVolume() != nil else { return false }
        let alert = NSAlert()
        alert.messageText = "Move Shenzhen KVM to Applications?"
        alert.informativeText = "Shenzhen KVM is running from its disk image. Once it’s in Applications, the disk image is ejected."
        alert.addButton(withTitle: "Move to Applications")
        alert.addButton(withTitle: "Not Now")
        NSApp.activate(ignoringOtherApps: true)
        guard alert.runModal() == .alertFirstButtonReturn else { return false }
        let fm = FileManager.default, target = URL(fileURLWithPath: "/Applications").appendingPathComponent(appName)
        do {
            if fm.fileExists(atPath: target.path) { try fm.trashItem(at: target, resultingItemURL: nil) }
            try fm.copyItem(at: Bundle.main.bundleURL, to: target)
        } catch {
            let failed = NSAlert()
            failed.messageText = "Shenzhen KVM couldn’t be moved to Applications"
            failed.informativeText = "Drag it onto the Applications folder instead. (\(error.localizedDescription))"
            failed.runModal()
            return false
        }
        let config = NSWorkspace.OpenConfiguration()
        config.createsNewApplicationInstance = true
        NSWorkspace.shared.openApplication(at: target, configuration: config) { _, _ in
            DispatchQueue.main.async { NSApp.terminate(nil) }
        }
        return true
    }

    /// Running from Applications: eject any Shenzhen KVM disk image still mounted, which also closes its Finder window.
    /// Retries for a few seconds, while a copy that was running from it quits.
    static func ejectInstallerDisk() {
        guard runningVolume() == nil else { return }
        let volumes = installerVolumes()
        guard !volumes.isEmpty else { return }
        DispatchQueue.global().async {
            for volume in volumes {
                for _ in 0..<10 {
                    if (try? NSWorkspace.shared.unmountAndEjectDevice(at: volume)) != nil { break }
                    Thread.sleep(forTimeInterval: 1)
                }
            }
        }
    }
}

// MARK: - Updates

/// Checks GitHub once a day (and on Check for Updates…), downloads ShenzhenKVM.dmg, checks it is ours (Developer ID
/// team, notarized, hardened runtime, bundle ID, published version), then swaps the app in place and reopens it.
enum Update {
    static let repo = "casimir-engineering/shenzhen-kvm"
    static let asset = "ShenzhenKVM.dmg"
    static let team = "66LJ4BV7Q3"
    static let bundleID = "com.intuition.shenzhenkvm"
    static let appName = "Shenzhen KVM"
    static let agentLabel = "com.intuition.shenzhenkvm.agent"
    static let agentPlist = "com.intuition.shenzhenkvm.agent.plist"
    static let releasesPage = URL(string: "https://github.com/\(repo)/releases/latest")!
    static let maxDownload = 64 * 1024 * 1024
    static let requirement = "anchor apple generic and certificate leaf[subject.OU] = \"\(team)\" and certificate 1[field.1.2.840.113635.100.6.2.6] exists and certificate leaf[field.1.2.840.113635.100.6.1.13] exists"
    static var d: UserDefaults { .standard }
    static var userAgent: String { "ShenzhenKVM/\(currentVersion) (macOS; +https://github.com/\(repo))" }

    struct Release { let tag: String; let version: String; let url: URL; let size: Int; let digest: String?; let notes: String }

    enum Failure: Error, CustomStringConvertible {
        case message(String)
        var description: String { if case .message(let m) = self { return m }; return "" }
    }

    static var currentVersion: String { updaterAppBundle.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String ?? "" }

    /// "v1.2.3" or "1.2.3" as numbers; nil when not all numbers.
    static func numbers(_ v: String) -> [Int]? {
        let t = v.hasPrefix("v") ? String(v.dropFirst()) : v
        let parts = t.split(separator: ".").map { Int($0) }
        return parts.isEmpty || parts.contains(nil) ? nil : parts.map { $0! }
    }
    static func compare(_ a: String, _ b: String) -> Int {
        guard let x = numbers(a), let y = numbers(b) else { return 0 }
        for i in 0..<max(x.count, y.count) {
            let p = i < x.count ? x[i] : 0, q = i < y.count ? y[i] : 0
            if p != q { return p < q ? -1 : 1 }
        }
        return 0
    }

    static var autoUpdate: Bool {
        get { d.object(forKey: "autoUpdate") as? Bool ?? true }
        set { d.set(newValue, forKey: "autoUpdate") }
    }

    /// Where the app runs from can't be replaced: a disk image or a translocated copy.
    static var cantUpdateHere: Bool {
        let path = updaterAppBundle.bundlePath
        return path.hasPrefix("/Volumes/") || path.contains("/AppTranslocation/") || path.hasPrefix("/private/var/folders")
    }

    static func latestRelease(conditional: Bool) throws -> Release? {
        var request = URLRequest(url: URL(string: "https://api.github.com/repos/\(repo)/releases/latest")!, timeoutInterval: 15)
        request.setValue(userAgent, forHTTPHeaderField: "User-Agent")
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("2022-11-28", forHTTPHeaderField: "X-GitHub-Api-Version")
        if conditional, let etag = d.string(forKey: "updateETag") { request.setValue(etag, forHTTPHeaderField: "If-None-Match") }
        let config = URLSessionConfiguration.ephemeral
        config.waitsForConnectivity = false
        let (data, response, error) = URLSession(configuration: config).updSyncData(request)
        guard error == nil, let http = response as? HTTPURLResponse
        else { throw Failure.message("Couldn’t check for updates. Check that this Mac is online, then try again.") }
        if http.statusCode == 304 { return nil }
        guard http.statusCode == 200, let data else { throw Failure.message("Couldn’t check for updates right now. Try again later.") }
        if let etag = http.value(forHTTPHeaderField: "ETag") { d.set(etag, forKey: "updateETag") }
        guard let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any], let tag = json["tag_name"] as? String,
              numbers(tag) != nil else { throw Failure.message("Couldn’t check for updates right now. Try again later.") }
        if json["draft"] as? Bool == true || json["prerelease"] as? Bool == true { return nil }
        guard let a = (json["assets"] as? [[String: Any]])?.first(where: { $0["name"] as? String == asset }),
              let link = (a["browser_download_url"] as? String).flatMap(URL.init(string:)) else { return nil }
        let version = tag.hasPrefix("v") ? String(tag.dropFirst()) : tag
        return Release(tag: tag, version: version, url: link, size: a["size"] as? Int ?? 0,
                       digest: (a["digest"] as? String)?.replacingOccurrences(of: "sha256:", with: ""),
                       notes: json["body"] as? String ?? "")
    }

    /// A release worth offering: newer than this one and never older than the newest seen (no downgrades).
    static func offerable(_ r: Release) -> Bool {
        let highest = d.string(forKey: "highestVersionSeen") ?? currentVersion
        if compare(r.version, highest) > 0 { d.set(r.version, forKey: "highestVersionSeen") }
        return compare(r.version, currentVersion) > 0 && compare(r.version, highest) >= 0
    }

    /// The release notes for the prompt: up to the first ---, list items as •, plain text, at most 500 characters.
    static func shortNotes(_ body: String) -> String {
        var text = body.components(separatedBy: "\n---").first ?? body
        text = text.split(separator: "\n", omittingEmptySubsequences: false).map { line -> String in
            var l = line.trimmingCharacters(in: .whitespaces)
            if l.hasPrefix("- ") || l.hasPrefix("* ") { l = "• " + l.dropFirst(2) }
            while l.hasPrefix("#") { l = String(l.dropFirst()) }
            return l.replacingOccurrences(of: "`", with: "").trimmingCharacters(in: .whitespaces)
        }.joined(separator: "\n").trimmingCharacters(in: .whitespacesAndNewlines)
        text = String(text.unicodeScalars.filter { !(0x202A...0x202E).contains($0.value) && !(0x2066...0x2069).contains($0.value)
                                                     && ($0.value >= 32 || $0 == "\n") })
        return text.count > 500 ? String(text.prefix(500)) + "\n…" : text
    }

    // MARK: Checking the download

    /// nil when the code at `url` is ours; otherwise why not (not shown, the user sees a general message).
    static func verify(_ url: URL, notarized: Bool, app: Bool) -> String? {
        var code: SecStaticCode?
        guard SecStaticCodeCreateWithPath(url as CFURL, [], &code) == errSecSuccess, let code else { return "unreadable" }
        var req: SecRequirement?
        guard SecRequirementCreateWithString((requirement + (notarized ? " and notarized" : "")) as CFString, [], &req) == errSecSuccess
        else { return "requirement" }
        let flags = app ? SecCSFlags(rawValue: kSecCSCheckAllArchitectures | kSecCSCheckNestedCode | kSecCSStrictValidate) : []
        guard SecStaticCodeCheckValidityWithErrors(code, flags, req, nil) == errSecSuccess else { return "signature or notarization" }
        var info: CFDictionary?
        guard SecCodeCopySigningInformation(code, SecCSFlags(rawValue: kSecCSSigningInformation), &info) == errSecSuccess,
              let dict = info as? [String: Any] else { return "signing information" }
        guard dict[kSecCodeInfoTeamIdentifier as String] as? String == team else { return "team" }
        if app {
            let codeFlags = (dict[kSecCodeInfoFlags as String] as? NSNumber)?.uint32Value ?? 0
            guard codeFlags & 0x10000 != 0 else { return "hardened runtime" }
            guard (dict[kSecCodeInfoPList as String] as? [String: Any])?["CFBundleIdentifier"] as? String == bundleID else { return "bundle" }
        }
        return nil
    }

    static func run(_ tool: String, _ args: [String]) -> (status: Int32, output: Data) {
        let p = Process(), pipe = Pipe()
        p.executableURL = URL(fileURLWithPath: tool); p.arguments = args
        p.standardOutput = pipe; p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return (-1, Data()) }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return (p.terminationStatus, data)
    }

    /// Starts `path` in a session of its own, so it outlives this app even when launchd started the app as an agent
    /// (launchd stops what is left of an agent's process group when the agent quits).
    static func spawnDetached(_ path: String, _ args: [String]) -> Bool {
        var attr: posix_spawnattr_t?
        posix_spawnattr_init(&attr)
        defer { posix_spawnattr_destroy(&attr) }
        posix_spawnattr_setflags(&attr, Int16(POSIX_SPAWN_SETSID))
        let argv: [UnsafeMutablePointer<CChar>?] = ([path] + args).map { strdup($0) } + [nil]
        defer { argv.forEach { free($0) } }
        var pid: pid_t = 0
        return posix_spawn(&pid, path, nil, &attr, argv, environ) == 0
    }

    /// Checks the downloaded disk image and takes the app out of it, checked too, into `staging`. Returns the app.
    static func prepare(dmg: URL, release: Release, staging: URL) throws -> URL {
        let refused = "The download didn’t pass Shenzhen KVM’s safety checks, so it wasn’t installed."
        let size = (try? FileManager.default.attributesOfItem(atPath: dmg.path)[.size] as? Int) ?? -1
        if release.size > 0, size != release.size { throw Failure.message("The update didn’t download completely. Try again.") }
        if let digest = release.digest, let data = try? Data(contentsOf: dmg) {
            let hex = SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
            if hex != digest.lowercased() { throw Failure.message("The update was damaged while downloading. Try again.") }
        }
        guard verify(dmg, notarized: true, app: false) == nil else { throw Failure.message(refused) }
        let attach = run("/usr/bin/hdiutil", ["attach", "-nobrowse", "-readonly", "-noautoopen", "-plist", dmg.path])
        guard attach.status == 0, let plist = try? PropertyListSerialization.propertyList(from: attach.output, format: nil) as? [String: Any]
        else { throw Failure.message("The downloaded update couldn’t be opened. Try again.") }
        guard let mount = (plist["system-entities"] as? [[String: Any]])?.compactMap({ $0["mount-point"] as? String }).first
        else { throw Failure.message("The downloaded update couldn’t be opened. Try again.") }
        defer { if run("/usr/bin/hdiutil", ["detach", mount]).status != 0 { _ = run("/usr/bin/hdiutil", ["detach", "-force", mount]) } }
        let fm = FileManager.default
        let apps = ((try? fm.contentsOfDirectory(atPath: mount)) ?? []).filter { name in
            let path = (mount as NSString).appendingPathComponent(name)
            return name.hasSuffix(".app") && (try? fm.destinationOfSymbolicLink(atPath: path)) == nil
                && Bundle(path: path)?.bundleIdentifier == bundleID
        }
        guard apps.count == 1 else { throw Failure.message("The download didn’t contain Shenzhen KVM, so nothing was installed.") }
        let staged = staging.appendingPathComponent(Installer.appName)
        guard run("/usr/bin/ditto", [(mount as NSString).appendingPathComponent(apps[0]), staged.path]).status == 0
        else { throw Failure.message("The update couldn’t be unpacked. Try again.") }
        guard verify(staged, notarized: false, app: true) == nil else { throw Failure.message(refused) }
        guard Bundle(url: staged)?.object(forInfoDictionaryKey: "CFBundleShortVersionString") as? String == release.version
        else { throw Failure.message(refused) }
        _ = run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", staged.path])
        return staged
    }

    /// A folder for the update beside the app, on its volume, so the swap is a rename.
    static func stagingFolder() throws -> URL {
        let parent = updaterAppBundle.bundleURL.deletingLastPathComponent()
        guard FileManager.default.isWritableFile(atPath: parent.path) else {
            throw Failure.message("Shenzhen KVM can’t replace itself in “\(parent.lastPathComponent)”. Download the new version and drag it there instead.")
        }
        do { return try FileManager.default.url(for: .itemReplacementDirectory, in: .userDomainMask, appropriateFor: parent, create: true) }
        catch { throw Failure.message("Shenzhen KVM couldn’t prepare the update. Download the new version and drag it to “\(parent.lastPathComponent)” instead.") }
    }

    /// The login agent is registered and on: launchd runs the app, so the update restarts it there (never a second copy).
    static func agentEnabled() -> Bool { SMAppService.agent(plistName: agentPlist).status == .enabled }

    /// Run as `shenzhen-kvm --post-update <staged.app> <target.app> <pid>`, after the app quits: swaps the app (keeping
    /// the old one beside it until the next launch), checks it again, reopens it. Puts the old one back on failure.
    static func postUpdate(staged: URL, target: URL, pid: pid_t) -> Never {
        setsid()
        let fm = FileManager.default, old = URL(fileURLWithPath: target.path + ".old")
        let viaAgent = agentEnabled()       // asked before the swap, while this bundle is still where it was registered
        func reopen(_ url: URL) {
            if viaAgent, run("/bin/launchctl", ["kickstart", "-k", "gui/\(getuid())/\(agentLabel)"]).status == 0 { return }
            let config = NSWorkspace.OpenConfiguration(); config.createsNewApplicationInstance = true
            let done = DispatchSemaphore(value: 0)
            NSWorkspace.shared.openApplication(at: url, configuration: config) { _, _ in done.signal() }
            _ = done.wait(timeout: .now() + 20)
        }
        func giveUp() -> Never {
            try? fm.removeItem(at: staged)
            d.removeObject(forKey: "updatePendingVersion")
            reopen(target)
            exit(1)
        }
        for _ in 0..<40 where kill(pid, 0) == 0 { usleep(500_000) }       // wait up to 20 s for the app to quit
        guard kill(pid, 0) != 0, verify(staged, notarized: false, app: true) == nil else { giveUp() }
        usleep(500_000)
        try? fm.removeItem(at: old)
        guard rename(target.path, old.path) == 0 else { giveUp() }
        guard rename(staged.path, target.path) == 0 else { _ = rename(old.path, target.path); giveUp() }
        guard verify(target, notarized: false, app: true) == nil else {
            try? fm.removeItem(at: target); _ = rename(old.path, target.path); giveUp()
        }
        _ = run("/usr/bin/xattr", ["-dr", "com.apple.quarantine", target.path])
        LSRegisterURL(target as CFURL, true)
        reopen(target)
        exit(0)
    }

    /// At launch: says when an update just finished; removes the previous app kept from an update.
    static func afterLaunch() -> String? {
        let old = updaterAppBundle.bundlePath + ".old"
        var finished: String?
        if let pending = d.string(forKey: "updatePendingVersion") {
            d.removeObject(forKey: "updatePendingVersion")
            if compare(pending, currentVersion) == 0 { finished = pending; try? FileManager.default.removeItem(atPath: old) }
        } else if let made = (try? FileManager.default.attributesOfItem(atPath: old))?[.modificationDate] as? Date,
                  Date().timeIntervalSince(made) > 3600 {
            try? FileManager.default.removeItem(atPath: old)
        }
        return finished
    }
}

extension URLSession {
    /// A request run to its end, for code already off the main thread.
    func updSyncData(_ request: URLRequest) -> (Data?, URLResponse?, Error?) {
        var result: (Data?, URLResponse?, Error?) = (nil, nil, nil)
        let done = DispatchSemaphore(value: 0)
        dataTask(with: request) { result = ($0, $1, $2); done.signal() }.resume()
        done.wait()
        return result
    }
}

/// The window side: daily checks, the prompt, the download with its progress panel, and the swap.
final class Updater: NSObject, URLSessionDownloadDelegate {
    static let shared = Updater()
    private let queue = DispatchQueue(label: "update", qos: .utility)
    private var busy = false, timer: Timer?
    private var panel: NSPanel?, heading = NSTextField(labelWithString: ""), detail = NSTextField(labelWithString: "")
    private var bar = NSProgressIndicator()
    private var task: URLSessionDownloadTask?, release: Update.Release?, staging: URL?, cancelled = false
    private var pendingPrompt: Update.Release?

    func start() {
        if let done = Update.afterLaunch() {
            // Reopened by launchd after an update, the app has no window: bring it forward for this one message.
            NSApp.activate(ignoringOtherApps: true)
            let a = NSAlert(); a.messageText = "You’re now on Shenzhen KVM \(done)."; a.runModal()
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self.dailyCheck() }
        timer = Timer.scheduledTimer(withTimeInterval: 3600, repeats: true) { _ in self.dailyCheck() }
        timer?.tolerance = 600
        NSWorkspace.shared.notificationCenter.addObserver(forName: NSWorkspace.didWakeNotification, object: nil, queue: .main) { _ in self.dailyCheck() }
        NotificationCenter.default.addObserver(forName: NSApplication.didBecomeActiveNotification, object: nil, queue: .main) { _ in
            if let r = self.pendingPrompt { self.pendingPrompt = nil; self.prompt(r, manual: false) }
        }
    }

    /// Once a day at most: the time is noted before asking, so a crash still counts as the day's check.
    func dailyCheck() {
        guard Update.autoUpdate, !busy, !Update.cantUpdateHere else { return }
        let last = Update.d.double(forKey: "lastUpdateCheck"), now = Date().timeIntervalSince1970
        guard last == 0 || now - last >= 86400 || now < last - 60 else { return }
        Update.d.set(now, forKey: "lastUpdateCheck")
        check(manual: false)
    }

    /// From a menu, also the menu bar one, which doesn't bring the app forward: do it, so the answer shows in front.
    @objc func checkNow() { NSApp.activate(ignoringOtherApps: true); check(manual: true) }

    func check(manual: Bool) {
        if busy { if manual { say("An update is already in progress.") }; return }
        if Update.cantUpdateHere {
            if manual { say("To enable automatic updates, move Shenzhen KVM to your Applications folder.") }
            return
        }
        busy = true
        queue.async {
            let result = Result { try Update.latestRelease(conditional: !manual) }
            DispatchQueue.main.async {
                self.busy = false
                switch result {
                case .failure(let e): if manual { self.say(String(describing: e)) }
                case .success(let r):
                    guard let r, Update.offerable(r) else {
                        if manual { self.say("You’re up to date.", "Shenzhen KVM \(Update.currentVersion) is the latest version.") }
                        return
                    }
                    if !manual {
                        if Update.d.string(forKey: "skippedUpdateVersion") == r.version { return }
                        if Update.d.string(forKey: "deferredUpdateVersion") == r.version,
                           Date().timeIntervalSince1970 < Update.d.double(forKey: "remindUpdateAfter") { return }
                        if !NSApp.isActive { self.pendingPrompt = r; return }
                    }
                    self.prompt(r, manual: manual)
                }
            }
        }
    }

    func prompt(_ r: Update.Release, manual: Bool) {
        let a = NSAlert()
        a.messageText = "A new version of Shenzhen KVM is available"
        let notes = Update.shortNotes(r.notes)
        a.informativeText = "Shenzhen KVM \(r.version) is available — you have \(Update.currentVersion). Would you like to install it now?"
            + (notes.isEmpty ? "" : "\n\n" + notes)
        a.addButton(withTitle: "Install and Relaunch")
        a.addButton(withTitle: "Skip This Version")
        a.addButton(withTitle: "Later")
        switch a.runModal() {
        case .alertFirstButtonReturn: download(r)
        case .alertSecondButtonReturn: Update.d.set(r.version, forKey: "skippedUpdateVersion")
        default:
            Update.d.set(r.version, forKey: "deferredUpdateVersion")
            Update.d.set(Date().timeIntervalSince1970 + 7 * 86400, forKey: "remindUpdateAfter")
        }
    }

    func download(_ r: Update.Release) {
        guard r.url.scheme == "https" else { say("The update’s download link isn’t secure, so nothing was downloaded."); return }
        let folder: URL
        do { folder = try Update.stagingFolder() } catch {
            let a = NSAlert(); a.alertStyle = .warning; a.messageText = "Software Update"; a.informativeText = String(describing: error)
            a.addButton(withTitle: "Open Download Page"); a.addButton(withTitle: "Cancel")
            if a.runModal() == .alertFirstButtonReturn { NSWorkspace.shared.open(Update.releasesPage) }
            return
        }
        if let free = (try? folder.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey]))?.volumeAvailableCapacityForImportantUsage,
           r.size > 0, free < Int64(r.size * 3) {
            try? FileManager.default.removeItem(at: folder)
            say("There is not enough disk space to install the update."); return
        }
        busy = true; release = r; staging = folder; cancelled = false; redirects = 0
        showPanel(r)
        var request = URLRequest(url: r.url, timeoutInterval: 60)
        request.setValue(Update.userAgent, forHTTPHeaderField: "User-Agent")
        let session = URLSession(configuration: .ephemeral, delegate: self, delegateQueue: .main)
        task = session.downloadTask(with: request)
        task?.resume()
    }

    private var redirects = 0
    func urlSession(_ session: URLSession, task: URLSessionTask, willPerformHTTPRedirection response: HTTPURLResponse,
                    newRequest request: URLRequest, completionHandler: @escaping (URLRequest?) -> Void) {
        redirects += 1
        completionHandler(request.url?.scheme == "https" && redirects <= 5 ? request : nil)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didWriteData bytesWritten: Int64,
                    totalBytesWritten: Int64, totalBytesExpectedToWrite: Int64) {
        let ceiling = Int64(min(Update.maxDownload, (release?.size ?? 0) > 0 ? release!.size + 1 : Update.maxDownload))
        if totalBytesWritten > ceiling || totalBytesExpectedToWrite > Int64(Update.maxDownload) { downloadTask.cancel(); return }
        let total = totalBytesExpectedToWrite > 0 ? totalBytesExpectedToWrite : Int64(release?.size ?? 0)
        if total > 0 { bar.isIndeterminate = false; bar.doubleValue = Double(totalBytesWritten) / Double(total) }
        detail.stringValue = String(format: "%.1f MB of %.1f MB", Double(totalBytesWritten) / 1e6, Double(total) / 1e6)
    }

    func urlSession(_ session: URLSession, downloadTask: URLSessionDownloadTask, didFinishDownloadingTo location: URL) {
        guard let staging, let release else { return }
        let dmg = staging.appendingPathComponent(Update.asset)
        do { try FileManager.default.moveItem(at: location, to: dmg) } catch { finish(error: "The downloaded update couldn’t be saved. Try again."); return }
        detail.stringValue = "Checking the update…"
        bar.isIndeterminate = true; bar.startAnimation(nil)
        queue.async {
            let result = Result { try Update.prepare(dmg: dmg, release: release, staging: staging) }
            DispatchQueue.main.async {
                switch result {
                case .failure(let e): self.finish(error: String(describing: e))
                case .success(let staged): self.detail.stringValue = "Preparing update…"; self.install(staged, release)
                }
            }
        }
    }

    func urlSession(_ session: URLSession, task: URLSessionTask, didCompleteWithError error: Error?) {
        guard let error else { return }
        if cancelled { finish(error: nil) } else { finish(error: (error as NSError).code == NSURLErrorCancelled
            ? "The update didn’t download completely. Try again." : "The update couldn’t be downloaded. Check that this Mac is online, then try again.") }
    }

    func install(_ staged: URL, _ r: Update.Release) {
        panel?.orderOut(nil)
        let a = NSAlert()
        a.messageText = "Shenzhen KVM will close and reopen to finish updating."
        a.addButton(withTitle: "Install Now"); a.addButton(withTitle: "Cancel")
        guard a.runModal() == .alertFirstButtonReturn else { finish(error: nil); return }
        Update.d.set(r.version, forKey: "updatePendingVersion")
        Update.d.synchronize()
        guard let exe = updaterAppBundle.executablePath,
              Update.spawnDetached(exe, ["--post-update", staged.path, updaterAppBundle.bundlePath, String(getpid())]) else {
            Update.d.removeObject(forKey: "updatePendingVersion"); finish(error: "The update couldn’t be installed. Try again."); return
        }
        NSApp.terminate(nil)
    }

    @objc func cancelDownload() { cancelled = true; task?.cancel() }

    func finish(error: String?) {
        busy = false; panel?.orderOut(nil); panel = nil; task = nil
        if let staging { try? FileManager.default.removeItem(at: staging) }
        staging = nil
        if let error { say(error) }
    }

    func showPanel(_ r: Update.Release) {
        let p = NSPanel(contentRect: NSRect(x: 0, y: 0, width: 380, height: 120), styleMask: [.titled], backing: .buffered, defer: false)
        p.title = "Software Update"
        heading.stringValue = "Downloading Shenzhen KVM \(r.version)"
        heading.font = .boldSystemFont(ofSize: 13)
        detail.stringValue = "Starting…"; detail.textColor = .secondaryLabelColor
        bar = NSProgressIndicator(); bar.isIndeterminate = false; bar.minValue = 0; bar.maxValue = 1; bar.doubleValue = 0
        let cancel = NSButton(title: "Cancel", target: self, action: #selector(cancelDownload))
        let stack = NSStackView(views: [heading, bar, detail, cancel])
        stack.orientation = .vertical; stack.alignment = .leading
        stack.edgeInsets = NSEdgeInsets(top: 16, left: 20, bottom: 16, right: 20)
        bar.widthAnchor.constraint(equalToConstant: 340).isActive = true
        cancel.setContentHuggingPriority(.required, for: .horizontal)
        p.contentView = stack
        p.center(); p.makeKeyAndOrderFront(nil)
        panel = p
    }

    func say(_ title: String, _ text: String = "") {
        let a = NSAlert()
        if text.isEmpty && !title.hasPrefix("You’re") { a.alertStyle = .warning; a.messageText = "Software Update"; a.informativeText = title }
        else { a.messageText = title; a.informativeText = text }
        a.runModal()
    }

    @objc func toggleAutomatic(_ item: NSMenuItem) {
        Update.autoUpdate.toggle()
        item.state = Update.autoUpdate ? .on : .off
    }
}
