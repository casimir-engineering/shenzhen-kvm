import Foundation
import CryptoKit

func nowUs() -> UInt64 { UInt64(Date().timeIntervalSince1970 * 1_000_000) }
func mono() -> Double { Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW)) / 1e9 }

// MARK: crypto

final class Box {
    let key: SymmetricKey
    let peerPrefix: [UInt8]
    private var counter: UInt64 = 0
    private var lock = os_unfair_lock()
    private var highest: [UInt32: UInt64] = [:]   // per sender id, UDP replay guard
    private var hlock = os_unfair_lock()
    static let me: [UInt8] = Array(myID.prefix(4))

    init(key k: [UInt8], peer: [UInt8]) { key = SymmetricKey(data: k); peerPrefix = Array(peer.prefix(4)) }
    convenience init?(link: Link?) { guard let l = link else { return nil }; self.init(key: l.key, peer: l.peerID) }

    func seal(_ plain: [UInt8]) -> [UInt8] {
        os_unfair_lock_lock(&lock)
        counter = max(counter + 1, nowUs())
        let c = counter
        os_unfair_lock_unlock(&lock)
        var n = Box.me
        withUnsafeBytes(of: c.bigEndian) { n.append(contentsOf: $0) }
        let sb = try! ChaChaPoly.seal(plain, using: key, nonce: try! ChaChaPoly.Nonce(data: n))
        return [UInt8](sb.combined)
    }

    /// Returns plaintext if authentic. `replayCheck` enforces a strictly rising counter per sender.
    func open(_ pkt: [UInt8], replayCheck: Bool) -> [UInt8]? {
        guard pkt.count >= 28, Array(pkt[0..<4]) == peerPrefix,
              let sb = try? ChaChaPoly.SealedBox(combined: pkt),
              let p = try? ChaChaPoly.open(sb, using: key) else { return nil }
        if replayCheck {
            let id = UInt32(pkt[0]) << 24 | UInt32(pkt[1]) << 16 | UInt32(pkt[2]) << 8 | UInt32(pkt[3])
            var c: UInt64 = 0
            for k in 4..<12 { c = c << 8 | UInt64(pkt[k]) }
            os_unfair_lock_lock(&hlock); defer { os_unfair_lock_unlock(&hlock) }
            if let h = highest[id], c <= h { return nil }
            highest[id] = c
        }
        return [UInt8](p)
    }
}

// MARK: byte helpers

extension Array where Element == UInt8 {
    mutating func u16(_ v: UInt16) { append(UInt8(v >> 8)); append(UInt8(v & 0xff)) }
    mutating func i16(_ v: Int) { u16(UInt16(bitPattern: Int16(clamping: v))) }
    mutating func u32(_ v: UInt32) { for s in stride(from: 24, through: 0, by: -8) { append(UInt8((v >> UInt32(s)) & 0xff)) } }
    mutating func u64(_ v: UInt64) { for s in stride(from: 56, through: 0, by: -8) { append(UInt8((v >> UInt64(s)) & 0xff)) } }
}

struct Reader {
    let b: [UInt8]; var i = 0
    init(_ b: [UInt8]) { self.b = b }
    var left: Int { b.count - i }
    mutating func u8() -> UInt8? { guard left >= 1 else { return nil }; defer { i += 1 }; return b[i] }
    mutating func u16() -> UInt16? { guard left >= 2 else { return nil }; defer { i += 2 }; return UInt16(b[i]) << 8 | UInt16(b[i+1]) }
    mutating func i16() -> Int? { u16().map { Int(Int16(bitPattern: $0)) } }
    mutating func u64() -> UInt64? {
        guard left >= 8 else { return nil }
        var v: UInt64 = 0; for k in 0..<8 { v = v << 8 | UInt64(b[i+k]) }; i += 8; return v
    }
    mutating func bytes(_ n: Int) -> ArraySlice<UInt8>? { guard left >= n else { return nil }; defer { i += n }; return b[i..<i+n] }
}

// MARK: UDP

func makeAddr(_ host: String, _ port: UInt16) -> sockaddr_in? {
    var a = sockaddr_in()
    a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size)
    a.sin_family = sa_family_t(AF_INET)
    a.sin_port = port.bigEndian
    if inet_pton(AF_INET, host, &a.sin_addr) == 1 { return a }
    // hostname
    var res: UnsafeMutablePointer<addrinfo>?
    var hints = addrinfo(); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_DGRAM
    guard getaddrinfo(host, nil, &hints, &res) == 0, let r = res else { return nil }
    defer { freeaddrinfo(res) }
    r.pointee.ai_addr.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { a.sin_addr = $0.pointee.sin_addr }
    return a
}

final class UDP {
    let fd: Int32
    init?(port: UInt16) {
        fd = socket(AF_INET, SOCK_DGRAM, 0)
        guard fd >= 0 else { return nil }
        var one: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4)
        setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &one, 4)
        var tos: Int32 = 0xB8
        setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, 4)
        var svc: Int32 = 4  // SO_NET_SERVICE_TYPE (0x1116) = NET_SERVICE_TYPE_VO: Wi-Fi voice queue
        setsockopt(fd, SOL_SOCKET, 0x1116, &svc, 4)
        var a = sockaddr_in()
        a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
        a.sin_port = port.bigEndian; a.sin_addr.s_addr = INADDR_ANY
        let r = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        if r != 0 { close(fd); return nil }
    }
    func send(_ b: [UInt8], to addr: sockaddr_in) {
        var a = addr
        _ = b.withUnsafeBytes { p in
            withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                sendto(fd, p.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
        }
    }
    func recv(timeoutMs: Int32 = -1) -> ([UInt8], sockaddr_in)? {
        if timeoutMs >= 0 {
            var p = pollfd(fd: fd, events: Int16(POLLIN), revents: 0)
            if poll(&p, 1, timeoutMs) <= 0 { return nil }
        }
        var buf = [UInt8](repeating: 0, count: 2048)
        var a = sockaddr_in(); var l = socklen_t(MemoryLayout<sockaddr_in>.size)
        let n = buf.withUnsafeMutableBytes { p in
            withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) {
                recvfrom(fd, p.baseAddress, 2048, 0, $0, &l) } }
        }
        if n < 0 { return nil }
        return (Array(buf[0..<n]), a)
    }
}

// MARK: TCP

private func writeAll(_ fd: Int32, _ b: [UInt8]) -> Bool {
    var off = 0
    while off < b.count {
        let n = b.withUnsafeBytes { write(fd, $0.baseAddress! + off, b.count - off) }
        if n <= 0 { return false }
        off += n
    }
    return true
}
private func readAll(_ fd: Int32, _ n: Int) -> [UInt8]? {
    var b = [UInt8](repeating: 0, count: n); var off = 0
    while off < n {
        let r = b.withUnsafeMutableBytes { read(fd, $0.baseAddress! + off, n - off) }
        if r <= 0 { return nil }
        off += r
    }
    return b
}
func tcpSendFrame(_ fd: Int32, _ box: Box, _ plain: [UInt8]) -> Bool {
    let s = box.seal(plain)
    var f = [UInt8](); f.u32(UInt32(s.count)); f += s
    return writeAll(fd, f)
}
func tcpReadFrame(_ fd: Int32, _ box: Box) -> [UInt8]? {
    guard let h = readAll(fd, 4) else { return nil }
    let n = Int(UInt32(h[0]) << 24 | UInt32(h[1]) << 16 | UInt32(h[2]) << 8 | UInt32(h[3]))
    guard n >= 28, n < 1 << 31, let b = readAll(fd, n) else { return nil }
    return box.open(b, replayCheck: false)
}
private func setTimeouts(_ fd: Int32, _ secs: Int) {
    var tv = timeval(tv_sec: secs, tv_usec: 0)
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
    var one: Int32 = 1
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4)
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, 4)
}

/// Connected TCP socket to the peer, or nil. Caller closes it.
func tcpConnect(host: String, port: UInt16, connectTimeoutMs: Int32 = 800) -> Int32? {
    guard var a = makeAddr(host, port) else { return nil }
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { return nil }
    var ok = false
    defer { if !ok { close(fd) } }
    let fl = fcntl(fd, F_GETFL); _ = fcntl(fd, F_SETFL, fl | O_NONBLOCK)
    let r = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { connect(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
    if r != 0 {
        guard errno == EINPROGRESS else { return nil }
        var p = pollfd(fd: fd, events: Int16(POLLOUT), revents: 0)
        guard poll(&p, 1, connectTimeoutMs) == 1 else { return nil }
        var err: Int32 = 0; var l = socklen_t(4)
        getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &l)
        guard err == 0 else { return nil }
    }
    _ = fcntl(fd, F_SETFL, fl)
    setTimeouts(fd, 30)
    ok = true
    return fd
}

/// One request, one reply. Returns the reply plaintext.
func tcpRequest(host: String, port: UInt16, box: Box, _ plain: [UInt8], connectTimeoutMs: Int32 = 800) -> [UInt8]? {
    guard let fd = tcpConnect(host: host, port: port, connectTimeoutMs: connectTimeoutMs) else { return nil }
    defer { close(fd) }
    guard tcpSendFrame(fd, box, plain) else { return nil }
    return tcpReadFrame(fd, box)
}

func tcpServe(port: UInt16, box currentBox: @escaping () -> Box?, handler: @escaping ([UInt8], Int32) -> [UInt8]?) -> Bool {
    let fd = socket(AF_INET, SOCK_STREAM, 0)
    guard fd >= 0 else { return false }
    var one: Int32 = 1
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4)
    var a = sockaddr_in()
    a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET)
    a.sin_port = port.bigEndian; a.sin_addr.s_addr = INADDR_ANY
    let r = withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(fd, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }
    guard r == 0, listen(fd, 8) == 0 else { close(fd); return false }
    Thread.detachNewThread {
        while true {
            let c = accept(fd, nil, nil)
            if c < 0 { continue }
            Thread.detachNewThread {
                defer { close(c) }
                setTimeouts(c, 30)
                guard let box = currentBox(), let req = tcpReadFrame(c, box), let rep = handler(req, c) else { return }
                _ = tcpSendFrame(c, box, rep)
            }
        }
    }
    return true
}
