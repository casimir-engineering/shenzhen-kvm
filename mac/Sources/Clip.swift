import AppKit

/// CLIP_DATA body (kind byte + data) from the Mac pasteboard. Call on the main thread.
func readPasteboard() -> [UInt8] {
    let pb = NSPasteboard.general
    if let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty {
        return encodeFiles(urls)
    }
    if let png = pb.data(forType: .png) { return [2] + png }
    if let tiff = pb.data(forType: .tiff), let rep = NSBitmapImageRep(data: tiff),
       let png = rep.representation(using: .png, properties: [:]) { return [2] + png }
    if let s = pb.string(forType: .string) { return [1] + Array(s.utf8) }
    return [0]
}

/// Files and folders (as their files) -> CLIP_DATA body. Any thread.
func encodeFiles(_ urls: [URL]) -> [UInt8] {
    var entries: [(String, URL)] = []
    let fm = FileManager.default
    for u in urls {
        var isDir: ObjCBool = false
        guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { continue }
        if isDir.boolValue {
            let base = u.lastPathComponent
            let e = fm.enumerator(at: u, includingPropertiesForKeys: [.isRegularFileKey], options: [])
            while let f = e?.nextObject() as? URL {
                guard (try? f.resourceValues(forKeys: [.isRegularFileKey]))?.isRegularFile == true else { continue }
                let rel = String(f.standardizedFileURL.path.dropFirst(u.standardizedFileURL.path.count + 1))
                entries.append((base + "/" + rel, f))
            }
        } else {
            entries.append((u.lastPathComponent, u))
        }
    }
    var b: [UInt8] = [3]
    b.u16(UInt16(min(entries.count, 65535)))
    for (name, u) in entries.prefix(65535) {
        let data = (try? Data(contentsOf: u, options: .mappedIfSafe)) ?? Data()
        let n = Array(name.utf8)
        b.u16(UInt16(n.count)); b += n
        b.u64(UInt64(data.count)); b += data
    }
    return b
}

/// Total bytes of a file or of all files in a folder.
func fileTreeSize(_ u: URL) -> UInt64 {
    let fm = FileManager.default
    var isDir: ObjCBool = false
    guard fm.fileExists(atPath: u.path, isDirectory: &isDir) else { return 0 }
    if !isDir.boolValue { return UInt64((try? u.resourceValues(forKeys: [.fileSizeKey]))?.fileSize ?? 0) }
    var t: UInt64 = 0
    let e = fm.enumerator(at: u, includingPropertiesForKeys: [.isRegularFileKey, .fileSizeKey], options: [])
    while let f = e?.nextObject() as? URL {
        if let v = try? f.resourceValues(forKeys: [.isRegularFileKey, .fileSizeKey]), v.isRegularFile == true { t += UInt64(v.fileSize ?? 0) }
    }
    return t
}

/// Puts a CLIP_DATA body on the Mac pasteboard. Call on the main thread. Returns false if malformed.
@discardableResult
func writePasteboard(_ body: [UInt8]) -> Bool {
    var r = Reader(body)
    guard let kind = r.u8() else { return false }
    let pb = NSPasteboard.general
    switch kind {
    case 0:
        pb.clearContents(); return true
    case 1:
        guard let s = String(bytes: body.dropFirst(), encoding: .utf8) else { return false }
        pb.clearContents(); pb.setString(s, forType: .string); return true
    case 2:
        let png = Data(body.dropFirst())
        let item = NSPasteboardItem()
        item.setData(png, forType: .png)
        if let tiff = NSImage(data: png)?.tiffRepresentation { item.setData(tiff, forType: .tiff) }
        pb.clearContents(); pb.writeObjects([item]); return true
    case 3:
        guard let count = r.u16() else { return false }
        let fm = FileManager.default
        let fmt = DateFormatter(); fmt.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let root0 = receivedFilesRoot()
        var root = root0.appendingPathComponent(fmt.string(from: Date()))
        var k = 2
        while fm.fileExists(atPath: root.path) { root = root0.appendingPathComponent(fmt.string(from: Date()) + " \(k)"); k += 1 }
        var tops: [String] = []
        for _ in 0..<count {
            guard let nl = r.u16(), let nb = r.bytes(Int(nl)), let name = String(bytes: nb, encoding: .utf8),
                  let sz = r.u64(), let data = r.bytes(Int(sz)) else { return false }
            let parts = name.split(separator: "/", omittingEmptySubsequences: true).map(String.init)
            guard !name.hasPrefix("/"), !parts.isEmpty, !parts.contains(".."), !parts.contains(".") else { return false }
            let dest = parts.reduce(root) { $0.appendingPathComponent($1) }
            try? fm.createDirectory(at: dest.deletingLastPathComponent(), withIntermediateDirectories: true)
            guard (try? Data(data).write(to: dest)) != nil else { return false }
            if !tops.contains(parts[0]) { tops.append(parts[0]) }
        }
        try? fm.createDirectory(at: root, withIntermediateDirectories: true)
        pb.clearContents()
        pb.writeObjects(tops.map { root.appendingPathComponent($0) as NSURL })
        log("received \(count) file(s) into \(root.path)")
        return true
    default:
        return false
    }
}

func onMain<T>(_ f: () -> T) -> T { Thread.isMainThread ? f() : DispatchQueue.main.sync(execute: f) }
