import AppKit
import CryptoKit

/// The other computer's name as it reports itself (HELLO), or a plain phrase before it has.
private var otherNameValue = ""
private var otherNameLock = os_unfair_lock()
func setOtherName(_ n: String) { os_unfair_lock_lock(&otherNameLock); otherNameValue = n; os_unfair_lock_unlock(&otherNameLock) }
func otherComputer() -> String {
    os_unfair_lock_lock(&otherNameLock); defer { os_unfair_lock_unlock(&otherNameLock) }
    return otherNameValue.isEmpty ? "the other computer" : otherNameValue
}
/// Same, for the start of a sentence.
func OtherComputer() -> String { let o = otherComputer(); return o == "the other computer" ? "The other computer" : o }   // host names keep their case
/// Folder for files received: ~/Downloads/From <name>/
func receivedFilesRoot() -> URL {
    let n = otherComputer().replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: "-")
    return FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Downloads/From \(n)")
}

/// One clipboard transfer, shown in the window and the menu.
final class Transfer {
    enum State { case waiting, running, done, failed }
    let toPeer: Bool
    var what: String
    var total: UInt64
    var done: UInt64 = 0
    var state: State = .running
    var reason = ""
    var pasteWaiting = false
    var skipped = 0                 // files the receiving computer already had (not sent)
    let started = mono()
    var finished = 0.0
    init(toPeer: Bool, what: String, total: UInt64) { self.toPeer = toPeer; self.what = what; self.total = total }

    var direction: String { toPeer ? "This Mac → \(otherComputer())" : "\(OtherComputer()) → this Mac" }
    var speed: Double { let t = (state == .running ? mono() : finished) - started; return t > 0.2 ? Double(done) / t : 0 }
    var fraction: Double { total > 0 ? min(1, Double(done) / Double(total)) : (state == .done ? 1 : 0) }
    /// "2 already on workbench" / "2 already here", or "" when nothing was skipped.
    var skippedText: String { skipped == 0 ? "" : toPeer ? "\(skipped) already on \(otherComputer())" : "\(skipped) already here" }
}

func sizeText(_ b: UInt64) -> String {
    b >= 1 << 30 ? String(format: "%.1f GB", Double(b) / Double(1 << 30)) : b >= 1 << 20 ? String(format: "%.1f MB", Double(b) / Double(1 << 20))
        : b >= 1024 ? "\(b >> 10) KB" : "\(b) bytes"
}

/// Recent transfers (newest first), shared between threads.
final class TransferList {
    private var items: [Transfer] = []
    private var lock = os_unfair_lock()
    var onChange: (() -> Void)?
    private var lastNotify = 0.0

    func add(_ t: Transfer) -> Transfer {
        os_unfair_lock_lock(&lock); items.insert(t, at: 0); if items.count > 8 { items.removeLast() }; os_unfair_lock_unlock(&lock)
        changed(force: true); return t
    }
    func snapshot() -> [Transfer] { os_unfair_lock_lock(&lock); defer { os_unfair_lock_unlock(&lock) }; return items }
    func update(_ t: Transfer, _ f: (Transfer) -> Void, force: Bool = false) {
        os_unfair_lock_lock(&lock); f(t); os_unfair_lock_unlock(&lock); changed(force: force)
    }
    func finish(_ t: Transfer, ok: Bool, reason: String = "") {
        update(t, { $0.state = ok ? .done : .failed; $0.reason = reason; $0.finished = mono(); if ok { $0.done = $0.total } }, force: true)
        log("\(t.direction): \(t.what), \(sizeText(t.total)): \(ok ? "done" : "failed: " + reason)")
    }
    /// The newest file transfer coming from the other computer that has not finished.
    func incoming() -> Transfer? { snapshot().first { !$0.toPeer && ($0.state == .waiting || $0.state == .running) } }
    private func changed(force: Bool) {
        let now = mono()
        if !force && now - lastNotify < 0.15 { return }
        lastNotify = now
        DispatchQueue.main.async { self.onChange?() }
    }
}

func filesSummary(_ names: [String]) -> String {
    let tops = names.reduce(into: [String]()) { r, n in let t = String(n.split(separator: "/").first ?? Substring(n)); if !r.contains(t) { r.append(t) } }
    if tops.count == 1 { return tops[0] }
    return "\(tops.count) items: " + tops.prefix(3).joined(separator: ", ") + (tops.count > 3 ? "…" : "")
}

// MARK: files in pieces (PROTOCOL.md FILES_BEGIN / FILES_DATA)

struct FileEntry { let rel: String; let url: URL; let size: UInt64 }

func listFiles(_ urls: [URL]) -> [FileEntry] {
    var out: [FileEntry] = []
    let fm = FileManager.default
    for u in urls {
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { continue }
        if isDir.boolValue {
            let base = u.lastPathComponent
            let e = fm.enumerator(at: u, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey], options: [])
            while let f = e?.nextObject() as? URL {
                guard let v = try? f.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]), v.isRegularFile == true else { continue }
                let rel = String(f.standardizedFileURL.path.dropFirst(u.standardizedFileURL.path.count + 1))
                out.append(FileEntry(rel: base + "/" + rel, url: f, size: UInt64(v.fileSize ?? 0)))
            }
        } else {
            out.append(FileEntry(rel: u.lastPathComponent, url: u, size: UInt64((try? u.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0)))
        }
    }
    return Array(out.prefix(65535))
}

/// SHA-256 of a file, read in pieces (never whole in memory).
func sha256File(_ url: URL) -> [UInt8]? {
    guard let fh = try? FileHandle(forReadingFrom: url) else { return nil }
    defer { try? fh.close() }
    var h = SHA256()
    while let d = try? fh.read(upToCount: 1 << 20), !d.isEmpty { h.update(data: d) }
    return Array(h.finalize())
}

// MARK: kept copies of received files (PROTOCOL.md Kept copies)

let markerName = ".shenzhen-kvm.json"
let keptCopies = 5
private let datedName = try! NSRegularExpression(pattern: #"^\d{4}-\d{2}-\d{2} \d{2}\.\d{2}\.\d{2}( \(?\d+\)?)?$"#)
private func isDatedName(_ n: String) -> Bool { datedName.firstMatch(in: n, range: NSRange(n.startIndex..., in: n)) != nil }

/// The folders this app made under root (date-time name and our marker), oldest first.
func keptFolders(_ root: URL) -> [(url: URL, files: [[String: Any]])] {
    let fm = FileManager.default
    var out: [(URL, Double, [[String: Any]])] = []
    for u in (try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey])) ?? [] {
        guard isDatedName(u.lastPathComponent), let v = try? u.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey]),
              v.isDirectory == true, v.isSymbolicLink != true,
              let d = try? Data(contentsOf: u.appendingPathComponent(markerName)),
              let j = try? JSONSerialization.jsonObject(with: d) as? [String: Any], j["by"] as? String == "shenzhen-kvm" else { continue }
        out.append((u, j["created"] as? Double ?? 0, j["files"] as? [[String: Any]] ?? []))
    }
    return out.sorted { $0.1 < $1.1 }.map { ($0.0, $0.2) }
}
private func mtime(_ u: URL) -> Double? { ((try? FileManager.default.attributesOfItem(atPath: u.path))?[.modificationDate] as? Date)?.timeIntervalSince1970 }
private func fsize(_ u: URL) -> UInt64? { (try? FileManager.default.attributesOfItem(atPath: u.path))?[.size] as? UInt64 }
/// Marks dir as ours and records what it holds.
func writeMarker(_ dir: URL, _ entries: [(rel: String, size: UInt64, sha: [UInt8])]) {
    let files: [[String: Any]] = entries.map { e in
        let u = e.rel.split(separator: "/").reduce(dir) { $0.appendingPathComponent(String($1)) }
        return ["path": e.rel, "size": e.size, "sha256": hex(e.sha), "mtime": mtime(u) ?? 0]
    }
    let j: [String: Any] = ["by": "shenzhen-kvm", "v": 1, "created": Date().timeIntervalSince1970, "files": files]
    try? JSONSerialization.data(withJSONObject: j, options: [.sortedKeys]).write(to: dir.appendingPathComponent(markerName))
}
/// Files held in the kept copies, unchanged since they arrived: "sha256:size" -> file.
func heldFiles(_ root: URL) -> [String: URL] {
    var m: [String: URL] = [:]
    for f in keptFolders(root) {
        for e in f.files {
            guard let p = e["path"] as? String, let sha = e["sha256"] as? String, let size = (e["size"] as? NSNumber)?.uint64Value else { continue }
            let parts = p.split(separator: "/").map(String.init)
            guard !parts.contains(".."), !p.hasPrefix("/") else { continue }
            let u = parts.reduce(f.url) { $0.appendingPathComponent($1) }
            if fsize(u) == size, let t = mtime(u), abs(t - ((e["mtime"] as? Double) ?? -1)) < 0.001 { m["\(sha):\(size)"] = u }
        }
    }
    return m
}
/// Keeps the newest kept copies; never one that a protected file is in. Removes root when it is empty.
func rollKeptCopies(_ root: URL, protect: [URL]) {
    let fm = FileManager.default
    let folders = keptFolders(root)
    var excess = folders.count - keptCopies
    for f in folders where excess > 0 {
        let p = f.url.standardizedFileURL.path + "/"
        if protect.contains(where: { ($0.standardizedFileURL.path + "/").hasPrefix(p) }) { continue }
        if (try? fm.removeItem(at: f.url)) != nil { excess -= 1; log("removed an older copy of received files (\(f.url.lastPathComponent))") }
    }
    let rest = (try? fm.contentsOfDirectory(atPath: root.path)) ?? ["?"]
    if rest.allSatisfy({ $0 == ".DS_Store" }) { try? fm.removeItem(at: root) }
}
/// Files on the clipboard now (they keep their folder).
func pasteboardFiles() -> [URL] {
    onMain { () -> [URL] in
        // Our own copy: known without reading it (reading would hand over the data now, and the real
        // paste later would not be noticed).
        if let mine = PasteWatch.shared.ownFiles(NSPasteboard.general) { return mine }
        return (NSPasteboard.general.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL]) ?? []
    }
}
/// A fresh dated folder under root.
func newReceivedFolder(_ root: URL) -> URL {
    let fm = FileManager.default
    let fmt = DateFormatter(); fmt.dateFormat = "yyyy-MM-dd HH.mm.ss"
    var dir = root.appendingPathComponent(fmt.string(from: Date()))
    var k = 2
    while fm.fileExists(atPath: dir.path) { dir = root.appendingPathComponent(fmt.string(from: Date()) + " \(k)"); k += 1 }
    try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
    return dir
}

// MARK: sending (PROTOCOL.md FILES_OFFER, FILES_BEGIN as the fallback)

/// Sends files; connect() opens a connection to the other computer. progress(sent, toSend), skipped(n).
/// Returns nil on success or the reason.
func sendFiles(connect: () -> Int32?, box: Box, files: [FileEntry], progress: (UInt64, UInt64) -> Void, skipped: (Int) -> Void) -> String? {
    var hashes: [[UInt8]] = []
    for f in files { guard let h = sha256File(f.url) else { return "could not read \(f.rel)" }; hashes.append(h) }
    guard let fd = connect() else { return "\(otherComputer()) did not answer" }
    var offer: [UInt8] = [0x2A]; offer.u16(UInt16(files.count))
    for (f, h) in zip(files, hashes) { let n = Array(f.rel.utf8); offer.u16(UInt16(n.count)); offer += n; offer.u64(f.size); offer += h }
    let noOffer = ProcessInfo.processInfo.environment["SHENZHEN_KVM_NO_OFFER"] == "1"   // tests: an older version
    guard noOffer || tcpSendFrame(fd, box, offer) else { close(fd); return "\(otherComputer()) did not take the files" }
    guard !noOffer, let have = tcpReadFrame(fd, box), have.first == 0x2B else {
        close(fd)   // an older Shenzhen KVM: send everything
        guard let fd2 = connect() else { return "\(otherComputer()) did not answer" }
        defer { close(fd2) }
        let total = files.reduce(UInt64(0)) { $0 + $1.size }
        return sendData(fd: fd2, box: box, files: files, header: true, ack: 0x27) { progress($0, total) }
    }
    defer { close(fd) }
    var r = Reader(Array(have.dropFirst())); var held = Set<Int>()
    for _ in 0..<(r.u16() ?? 0) { if let i = r.u16() { held.insert(Int(i)) } }
    skipped(held.count)
    let rest = files.enumerated().filter { !held.contains($0.offset) }.map { $0.element }
    let total = rest.reduce(UInt64(0)) { $0 + $1.size }
    progress(0, total)
    return sendData(fd: fd, box: box, files: rest, header: false, ack: 0x2A) { progress($0, total) }
}
/// FILES_BEGIN (when header) and the FILES_DATA frames of files, then waits for ACK(ack).
private func sendData(fd: Int32, box: Box, files: [FileEntry], header: Bool, ack: UInt8, progress: (UInt64) -> Void) -> String? {
    if header {
        var h: [UInt8] = [0x27]; h.u16(UInt16(files.count))
        for f in files { let n = Array(f.rel.utf8); h.u16(UInt16(n.count)); h += n; h.u64(f.size) }
        guard tcpSendFrame(fd, box, h) else { return "\(otherComputer()) did not take the files" }
    }
    var sent: UInt64 = 0
    let chunk = 1 << 20
    for f in files {
        guard let fh = try? FileHandle(forReadingFrom: f.url) else { return "could not read \(f.rel)" }
        defer { try? fh.close() }
        var left = f.size
        while left > 0 {
            let want = Int(min(UInt64(chunk), left))
            var d = (try? fh.read(upToCount: want)) ?? Data()
            if d.isEmpty { d = Data(count: want) }   // file shrank meanwhile: keep the promised size
            guard tcpSendFrame(fd, box, [0x28] + d) else { return "the connection to \(otherComputer()) was lost" }
            left -= UInt64(d.count); sent += UInt64(d.count); progress(sent)
        }
    }
    guard let r = tcpReadFrame(fd, box), r.first == 0x02 else { return "\(otherComputer()) could not save the files" }
    return nil
}

// MARK: receiving

struct ReceivedFiles { let items: [URL]; let folder: URL; let skipped: Int }

/// Receives files after FILES_OFFER (offer) or FILES_BEGIN into a new folder under
/// ~/Downloads/From <other computer>/. progress(names, got, toReceive, skipped).
func receiveFiles(fd: Int32, box: Box, header: [UInt8], offer: Bool, progress: ([String], UInt64, UInt64, Int) -> Void) -> Result<ReceivedFiles, Error> {
    struct Fail: Error, CustomStringConvertible { let description: String }
    var r = Reader(Array(header.dropFirst()))
    guard let count = r.u16() else { return .failure(Fail(description: "bad header")) }
    var entries: [(name: String, size: UInt64, sha: [UInt8]?)] = []
    for _ in 0..<count {
        guard let nl = r.u16(), let nb = r.bytes(Int(nl)), let name = String(bytes: nb, encoding: .utf8), let sz = r.u64() else { return .failure(Fail(description: "bad header")) }
        var sha: [UInt8]? = nil
        if offer { guard let h = r.bytes(32) else { return .failure(Fail(description: "bad header")) }; sha = Array(h) }
        let parts = name.split(separator: "/").map(String.init)
        guard !name.hasPrefix("/"), !parts.isEmpty, !parts.contains(".."), !parts.contains(".") else { return .failure(Fail(description: "refused file name \(name)")) }
        entries.append((name, sz, sha))
    }
    let root = receivedFilesRoot()
    // Files already held in the kept copies are not sent again.
    var from: [Int: URL] = [:]
    if offer {
        let held = heldFiles(root)
        for (i, e) in entries.enumerated() { if let sha = e.sha, let u = held["\(hex(sha)):\(e.size)"] { from[i] = u } }
        var have: [UInt8] = [0x2B]; have.u16(UInt16(from.count)); for i in from.keys.sorted() { have.u16(UInt16(i)) }
        guard tcpSendFrame(fd, box, have) else { return .failure(Fail(description: "the connection to \(otherComputer()) was lost")) }
    }
    let names = entries.map { $0.name }
    let total = entries.enumerated().filter { from[$0.offset] == nil }.reduce(UInt64(0)) { $0 + $1.element.size }
    progress(names, 0, total, from.count)
    let fm = FileManager.default
    try? fm.createDirectory(at: root, withIntermediateDirectories: true)
    let dir = newReceivedFolder(root)
    func fail(_ why: String) -> Result<ReceivedFiles, Error> { try? fm.removeItem(at: dir); return .failure(Fail(description: why)) }
    var got: UInt64 = 0
    var buf = Data(); var bufOff = 0
    func next(_ n: Int) -> Data? {   // n bytes from the FILES_DATA stream
        var out = Data()
        while out.count < n {
            if bufOff >= buf.count {
                guard let f = tcpReadFrame(fd, box), f.first == 0x28 else { return nil }
                buf = Data(f.dropFirst()); bufOff = 0
                got += UInt64(buf.count); progress(names, got, total, from.count)
            }
            let take = min(n - out.count, buf.count - bufOff)
            out.append(buf[buf.startIndex + bufOff ..< buf.startIndex + bufOff + take]); bufOff += take
        }
        return out
    }
    var tops: [String] = [], made: [(rel: String, size: UInt64, sha: [UInt8])] = []
    for (i, e) in entries.enumerated() {
        let dest = e.name.split(separator: "/").reduce(dir) { $0.appendingPathComponent(String($1)) }
        try? fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        if let src = from[i] {   // already here: a clone of the kept copy
            guard (try? fm.copyItem(at: src, to: dest)) != nil else { return fail("could not copy \(e.name)") }
            made.append((e.name, e.size, e.sha ?? []))
        } else {
            guard fm.createFile(atPath: dest.path, contents: nil), let fh = try? FileHandle(forWritingTo: dest) else { return fail("could not write \(e.name)") }
            var h = SHA256(); var left = e.size
            while left > 0 {
                let n = Int(min(left, 1 << 20))
                guard let d = next(n) else { try? fh.close(); return fail("the connection to \(otherComputer()) was lost") }
                fh.write(d); h.update(data: d); left -= UInt64(n)
            }
            try? fh.close()
            let sha = Array(h.finalize())
            if let want = e.sha, want != sha { log("refused \(e.name): its content does not match what was announced"); return fail("\(e.name) arrived damaged") }
            made.append((e.name, e.size, sha))
        }
        let top = String(e.name.split(separator: "/")[0]); if !tops.contains(top) { tops.append(top) }
    }
    writeMarker(dir, made)
    return .success(ReceivedFiles(items: tops.map { dir.appendingPathComponent($0) }, folder: dir, skipped: from.count))
}

// MARK: received files that get pasted are removed once the pasting app has read them

/// Received files go on the clipboard lazily, so this app learns when another app asks for them (a
/// paste). Then, once no other process has any of the copy's files open for 10 s, the copy is removed
/// from Downloads/From <name> and, if the clipboard still holds it, the clipboard is emptied (a second
/// paste would otherwise offer files that are gone). Copies never pasted are left to the rolling 5.
final class PasteWatch: NSObject, NSPasteboardItemDataProvider {
    static let shared = PasteWatch()
    private var items: [NSPasteboardItem] = []
    private var itemURL: [ObjectIdentifier: URL] = [:]
    private var current: URL?                         // the copy whose files are on the clipboard
    private var ownChange = -1
    private var watched: [URL: (requested: Double, lastBusy: Double)] = [:]
    private var timer: Timer?
    var selfReading = false                           // this app reading its own clipboard is not a paste

    /// Main thread.
    func put(_ urls: [URL], folder: URL, on pb: NSPasteboard) {
        items = urls.map { u in let it = NSPasteboardItem(); it.setDataProvider(self, forTypes: [.fileURL]); return it }
        itemURL = Dictionary(uniqueKeysWithValues: zip(items.map { ObjectIdentifier($0) }, urls))
        pb.clearContents(); pb.writeObjects(items)
        current = folder; ownChange = pb.changeCount
    }
    func pasteboard(_ pasteboard: NSPasteboard?, item: NSPasteboardItem, provideDataForType type: NSPasteboard.PasteboardType) {
        guard let u = itemURL[ObjectIdentifier(item)] else { return }
        item.setString(u.absoluteString, forType: .fileURL)
        guard !selfReading, let f = current else { return }
        let now = mono()
        if watched[f] == nil { log("received files asked for by another app (\(f.lastPathComponent)): removed once it has read them") }
        watched[f] = (now, watched[f]?.lastBusy ?? 0)
        if timer == nil { timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() } }
    }
    func pasteboardFinishedWithDataProvider(_ pasteboard: NSPasteboard) {}
    /// The files of our copy while the clipboard still holds it, else nil.
    func ownFiles(_ pb: NSPasteboard) -> [URL]? { pb.changeCount == ownChange ? Array(itemURL.values) : nil }

    private func tick() {
        let now = mono(), pb = NSPasteboard.general
        for (f, t) in watched {
            guard FileManager.default.fileExists(atPath: f.path) else { watched[f] = nil; continue }
            if PasteWatch.inUse(f) { watched[f] = (t.requested, now); continue }
            guard now - max(t.requested, t.lastBusy) >= 10 else { continue }
            try? FileManager.default.removeItem(at: f)
            watched[f] = nil
            log("removed pasted files (\(f.lastPathComponent))")
            if f == current && pb.changeCount == ownChange { pb.clearContents(); markOwnPasteboardChange?() }
            if f == current { current = nil }
            let root = f.deletingLastPathComponent()
            if ((try? FileManager.default.contentsOfDirectory(atPath: root.path)) ?? ["?"]).allSatisfy({ $0 == ".DS_Store" }) { try? FileManager.default.removeItem(at: root) }
        }
        if watched.isEmpty { timer?.invalidate(); timer = nil }
    }
    var markOwnPasteboardChange: (() -> Void)?

    /// Another process has one of the folder's files open (at most 2000 files are checked).
    static func inUse(_ folder: URL) -> Bool {
        typealias F = @convention(c) (UInt32, UInt32, UnsafePointer<CChar>, UInt32, UnsafeMutableRawPointer?, Int32) -> Int32
        guard let sym = dlsym(dlopen(nil, RTLD_NOW), "proc_listpidspath") else { return false }
        let listpids = unsafeBitCast(sym, to: F.self)
        let me = getpid()
        var buf = [Int32](repeating: 0, count: 256)
        let e = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: [.isRegularFileKey], options: [])
        var n = 0
        while let u = e?.nextObject() as? URL, n < 2000 {
            guard (try? u.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true else { continue }
            n += 1
            let got = buf.withUnsafeMutableBytes { listpids(1 /* PROC_ALL_PIDS */, 0, u.path, 0, $0.baseAddress, Int32($0.count)) }
            if got > 0 { for i in 0..<Int(got) / 4 where buf[i] != 0 && buf[i] != me { return true } }
        }
        return false
    }
}
