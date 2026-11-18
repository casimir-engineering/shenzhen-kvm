import AppKit

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
    let started = mono()
    var finished = 0.0
    init(toPeer: Bool, what: String, total: UInt64) { self.toPeer = toPeer; self.what = what; self.total = total }

    var direction: String { toPeer ? "This Mac → \(otherComputer())" : "\(OtherComputer()) → this Mac" }
    var speed: Double { let t = (state == .running ? mono() : finished) - started; return t > 0.2 ? Double(done) / t : 0 }
    var fraction: Double { total > 0 ? min(1, Double(done) / Double(total)) : (state == .done ? 1 : 0) }
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

/// Sends files over an open connection; calls progress with bytes sent. Returns nil on success or the reason.
func sendFiles(fd: Int32, box: Box, files: [FileEntry], progress: (UInt64) -> Void) -> String? {
    var h: [UInt8] = [0x27]; h.u16(UInt16(files.count))
    for f in files { let n = Array(f.rel.utf8); h.u16(UInt16(n.count)); h += n; h.u64(f.size) }
    guard tcpSendFrame(fd, box, h) else { return "\(otherComputer()) did not take the files" }
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

/// Receives files after a FILES_BEGIN header into ~/Downloads/From <other computer>/<time>/. Returns the top-level items.
func receiveFiles(fd: Int32, box: Box, header: [UInt8], progress: ([String], UInt64, UInt64) -> Void) -> Result<[URL], Error> {
    struct Fail: Error, CustomStringConvertible { let description: String }
    var r = Reader(Array(header.dropFirst()))
    guard let count = r.u16() else { return .failure(Fail(description: "bad header")) }
    var entries: [(String, UInt64)] = []
    for _ in 0..<count {
        guard let nl = r.u16(), let nb = r.bytes(Int(nl)), let name = String(bytes: nb, encoding: .utf8), let sz = r.u64() else { return .failure(Fail(description: "bad header")) }
        let parts = name.split(separator: "/").map(String.init)
        guard !name.hasPrefix("/"), !parts.isEmpty, !parts.contains(".."), !parts.contains(".") else { return .failure(Fail(description: "refused file name \(name)")) }
        entries.append((name, sz))
    }
    let total = entries.reduce(UInt64(0)) { $0 + $1.1 }
    let names = entries.map { $0.0 }
    progress(names, 0, total)
    let fm = FileManager.default
    let fmt = DateFormatter(); fmt.dateFormat = "yyyy-MM-dd HH.mm.ss"
    let root0 = receivedFilesRoot()
    var root = root0.appendingPathComponent(fmt.string(from: Date()))
    var k = 2
    while fm.fileExists(atPath: root.path) { root = root0.appendingPathComponent(fmt.string(from: Date()) + " \(k)"); k += 1 }
    var got: UInt64 = 0
    var buf = Data(); var bufOff = 0
    func next(_ n: Int) -> Data? {   // n bytes from the FILES_DATA stream
        var out = Data()
        while out.count < n {
            if bufOff >= buf.count {
                guard let f = tcpReadFrame(fd, box), f.first == 0x28 else { return nil }
                buf = Data(f.dropFirst()); bufOff = 0
                got += UInt64(buf.count); progress(names, got, total)
            }
            let take = min(n - out.count, buf.count - bufOff)
            out.append(buf[buf.startIndex + bufOff ..< buf.startIndex + bufOff + take]); bufOff += take
        }
        return out
    }
    var tops: [String] = []
    for (name, sz) in entries {
        let dest = name.split(separator: "/").reduce(root) { $0.appendingPathComponent(String($1)) }
        try? fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
        guard fm.createFile(atPath: dest.path, contents: nil), let fh = try? FileHandle(forWritingTo: dest) else { return .failure(Fail(description: "could not write \(name)")) }
        var left = sz
        while left > 0 {
            let n = Int(min(left, 1 << 20))
            guard let d = next(n) else { try? fh.close(); return .failure(Fail(description: "the connection to \(otherComputer()) was lost")) }
            fh.write(d); left -= UInt64(n)
        }
        try? fh.close()
        let top = String(name.split(separator: "/")[0]); if !tops.contains(top) { tops.append(top) }
    }
    try? fm.createDirectory(at: root, withIntermediateDirectories: true)
    return .success(tops.map { root.appendingPathComponent($0) })
}
