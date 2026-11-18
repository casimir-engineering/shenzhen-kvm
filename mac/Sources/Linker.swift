import Foundation
import CryptoKit

/// Linking two computers (PROTOCOL.md Linking): this Mac asks a visible computer ("asking", role L),
/// or answers a computer that asks while this Mac is visible ("asked", role V).
final class Linker {
    enum Stage: String { case none, connecting, waiting, confirm, done, failed }
    struct View { var stage: Stage = .none; var asked = false; var peerName = ""; var code = ""; var youConfirmed = false; var theyConfirmed = false; var message = "" }

    private var L = os_unfair_lock()
    func locked<T>(_ f: () -> T) -> T { os_unfair_lock_lock(&L); defer { os_unfair_lock_unlock(&L) }; return f() }
    private(set) var view = View()
    var onChange: (() -> Void)?
    var onLinked: ((Link) -> Void)?

    private var listenFD: Int32 = -1
    private var fd: Int32 = -1
    private var wake: [Int32] = [-1, -1]           // pipe: the person's answer
    private var answer: Bool?
    private var busy = false

    private func set(_ f: (inout View) -> Void) { locked { f(&view) }; DispatchQueue.main.async { self.onChange?() } }

    // MARK: asked (visible)

    func listen(_ on: Bool) {
        if on {
            guard listenFD < 0 else { return }
            let s = socket(AF_INET, SOCK_STREAM, 0); var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, 4)
            var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET); a.sin_port = discPort.bigEndian
            guard withUnsafePointer(to: &a, { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(s, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }) == 0, Darwin.listen(s, 4) == 0 else {
                log("cannot listen for linking on TCP \(discPort)"); close(s); return
            }
            listenFD = s
            Thread.detachNewThread { [weak self] in
                while let me = self, me.listenFD == s {
                    var p = pollfd(fd: s, events: Int16(POLLIN), revents: 0)
                    if poll(&p, 1, 500) <= 0 { continue }
                    let c = accept(s, nil, nil); if c < 0 { continue }
                    if me.locked({ me.busy }) { Linker.sendFrame(c, ["t": "busy"]); close(c); continue }
                    me.locked { me.busy = true }
                    me.run(fd: c, role: "V", found: nil)
                }
            }
        } else if listenFD >= 0 {
            let s = listenFD; listenFD = -1; close(s)
            let st = locked { view }
            if st.asked && (st.stage == .waiting || st.stage == .confirm) { cancel() }
        }
    }

    // MARK: asking

    func link(to f: Found) {
        guard locked({ !busy }) else { return }
        locked { busy = true }
        set { $0 = View(stage: .connecting, asked: false, peerName: f.name) }
        Thread.detachNewThread { [weak self] in
            guard let self else { return }
            guard let c = tcpConnect(host: f.ip, port: f.dp + 2, connectTimeoutMs: 3000) else {
                self.finish(failed: "\(f.name) could not be reached. Check that it is still visible."); return
            }
            self.run(fd: c, role: "L", found: f)
        }
    }

    /// The person's decision on the code (true = they match).
    func confirm(_ yes: Bool) {
        locked { answer = yes }
        if wake[1] >= 0 { var b: UInt8 = 1; _ = write(wake[1], &b, 1) }
    }
    func cancel() {
        let st = locked { view.stage }
        if st == .connecting || st == .waiting || st == .confirm { confirm(false) }
        else { set { $0 = View() } }
    }
    func clearResult() { if locked({ view.stage == .done || view.stage == .failed }) { set { $0 = View() } } }

    // MARK: the session

    private static func sendFrame(_ fd: Int32, _ j: [String: Any]) {
        let d = Array(try! JSONSerialization.data(withJSONObject: j)); var f = [UInt8](); f.u32(UInt32(d.count)); f += d
        _ = f.withUnsafeBytes { write(fd, $0.baseAddress, f.count) }
    }
    private func finish(failed msg: String?, done: String? = nil) {
        if fd >= 0 { close(fd); fd = -1 }
        for i in 0..<2 where wake[i] >= 0 { close(wake[i]); wake[i] = -1 }
        locked { busy = false; answer = nil }
        if let done { set { $0.stage = .done; $0.message = done } } else { set { $0.stage = .failed; $0.message = msg ?? "The link could not be made. Try again." } }
        log("linking: \(done ?? msg ?? "failed")")
        DispatchQueue.main.asyncAfter(deadline: .now() + 15) { [weak self] in self?.clearResult() }
    }

    private func run(fd c: Int32, role: String, found: Found?) {
        fd = c; pipe(&wake); locked { answer = nil }
        var tv = timeval(tv_sec: 1, tv_usec: 0); setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
        var one: Int32 = 1; setsockopt(c, SOL_SOCKET, SO_NOSIGPIPE, &one, 4)
        let deadline = mono() + 180
        var inbuf = [UInt8]()
        /// Next frame, or nil on close/timeout; also returns early (with ["answer": …]) when the person answers.
        func next() -> [String: Any]? {
            while mono() < deadline {
                if inbuf.count >= 4 {
                    let l = Int(UInt32(inbuf[0]) << 24 | UInt32(inbuf[1]) << 16 | UInt32(inbuf[2]) << 8 | UInt32(inbuf[3]))
                    if l > 8192 { return nil }
                    if inbuf.count >= 4 + l {
                        let j = try? JSONSerialization.jsonObject(with: Data(inbuf[4..<4 + l])) as? [String: Any]
                        inbuf.removeFirst(4 + l); return j
                    }
                }
                var p = [pollfd(fd: c, events: Int16(POLLIN), revents: 0), pollfd(fd: wake[0], events: Int16(POLLIN), revents: 0)]
                if poll(&p, 2, 500) <= 0 { continue }
                if p[1].revents != 0 { var b: UInt8 = 0; _ = read(wake[0], &b, 1); return ["answer": locked({ answer }) ?? false] }
                var b = [UInt8](repeating: 0, count: 4096)
                let n = read(c, &b, 4096); if n <= 0 { return nil }
                inbuf += b[0..<n]
            }
            return nil
        }
        func hexField(_ j: [String: Any], _ k: String, _ n: Int) -> [UInt8]? { (j[k] as? String).flatMap(unhex).flatMap { $0.count == n ? $0 : nil } }
        let other = { () -> String in self.locked { self.view.peerName.isEmpty ? "The other computer" : self.view.peerName } }

        let sk = Curve25519.KeyAgreement.PrivateKey()
        let pk = Array(sk.publicKey.rawRepresentation)
        var nMe = [UInt8](repeating: 0, count: 32); _ = SecRandomCopyBytes(kSecRandomDefault, 32, &nMe)
        var peerPK: [UInt8] = [], nPeer: [UInt8] = [], peerID: [UInt8] = [], peerName = "", peerDP: UInt16 = 24800
        let commitLabel = Array("shenzhen-kvm commit v1".utf8)

        if role == "L", let f = found {
            peerID = unhex(f.id) ?? []; peerName = f.name; peerDP = f.dp
            let commit = Array(SHA256.hash(data: commitLabel + pk + nMe))
            Linker.sendFrame(c, ["t": "request", "id": myIDHex, "name": computerName(), "dp": Int(portBase), "pub": hex(pk), "commit": hex(commit)])
            set { $0.stage = .waiting }
            guard let h = next(), h["t"] as? String == "hello", let pid = hexField(h, "id", 16), pid == peerID,
                  let ppk = hexField(h, "pub", 32), let np = hexField(h, "nonce", 32) else {
                return finish(failed: "\(other()) did not link.")
            }
            peerPK = ppk; nPeer = np; if let n = h["name"] as? String, !n.isEmpty { peerName = n }; peerDP = UInt16(h["dp"] as? Int ?? Int(peerDP))
            Linker.sendFrame(c, ["t": "reveal", "nonce": hex(nMe)])
        } else {
            set { $0 = View(stage: .waiting, asked: true) }
            guard let r = next(), r["t"] as? String == "request", let pid = hexField(r, "id", 16), let ppk = hexField(r, "pub", 32),
                  let commit = hexField(r, "commit", 32) else { return finish(failed: nil) }
            peerID = pid; peerPK = ppk; peerName = (r["name"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? "A computer"; peerDP = UInt16(r["dp"] as? Int ?? 24800)
            set { $0.peerName = peerName }
            Linker.sendFrame(c, ["t": "hello", "id": myIDHex, "name": computerName(), "dp": Int(portBase), "pub": hex(pk), "nonce": hex(nMe)])
            guard let v = next(), v["t"] as? String == "reveal", let np = hexField(v, "nonce", 32),
                  Array(SHA256.hash(data: commitLabel + ppk + np)) == commit else { return finish(failed: nil) }
            nPeer = np
        }

        // Derive the code, key and link id (pub/nonce/id ordered L, V).
        let (pkL, pkV, nL, nV, idL, idV) = role == "L" ? (pk, peerPK, nMe, nPeer, myID, peerID) : (peerPK, pk, nPeer, nMe, peerID, myID)
        let th = Array(SHA256.hash(data: Array("shenzhen-kvm link v1".utf8) + pkL + pkV + nL + nV + idL + idV))
        let ch = Array(SHA256.hash(data: Array("shenzhen-kvm code v1".utf8) + th))
        var v64: UInt64 = 0; for k in 0..<8 { v64 = v64 << 8 | UInt64(ch[k]) }
        let codeN = Int(v64 % 1_000_000), code = String(format: "%03d %03d", codeN / 1000, codeN % 1000)
        guard let ppub = try? Curve25519.KeyAgreement.PublicKey(rawRepresentation: peerPK),
              let shared = try? sk.sharedSecretFromKeyAgreement(with: ppub),
              shared.withUnsafeBytes({ $0.contains { $0 != 0 } }) else { return finish(failed: nil) }
        let key = shared.hkdfDerivedSymmetricKey(using: SHA256.self, salt: th, sharedInfo: Array("shenzhen-kvm key v1".utf8), outputByteCount: 32)
        let keyBytes = key.withUnsafeBytes { Array($0) }
        let linkID = Array(Array(SHA256.hash(data: Array("shenzhen-kvm link id v1".utf8) + th)).prefix(8))
        let matchLabel = Array("shenzhen-kvm match v1".utf8)
        func proof(_ r: String) -> [UInt8] { Array(HMAC<SHA256>.authenticationCode(for: matchLabel + Array(r.utf8), using: key)) }
        set { $0.stage = .confirm; $0.peerName = peerName; $0.code = code }
        log("linking with \(peerName): code \(code)")

        var meOK = false, peerOK = false
        while !(meOK && peerOK) {
            guard let f = next() else { return finish(failed: "\(peerName) stopped answering, so the computers were not linked.") }
            if let a = f["answer"] as? Bool {
                if !a { Linker.sendFrame(c, ["t": "cancel"]); return finish(failed: "You didn't link the computers.") }
                if !meOK { Linker.sendFrame(c, ["t": "match", "proof": hex(proof(role))]); meOK = true; set { $0.youConfirmed = true } }
            } else if f["t"] as? String == "cancel" {
                return finish(failed: "\(peerName) did not link.")
            } else if f["t"] as? String == "match" {
                guard let p = hexField(f, "proof", 32), p == proof(role == "L" ? "V" : "L") else { return finish(failed: "The codes did not match, so the computers were not linked.") }
                peerOK = true; set { $0.theyConfirmed = true }
            } else { return finish(failed: nil) }
        }
        let link = Link(peerID: peerID, peerName: peerName, key: keyBytes, linkID: linkID, peerDP: peerDP)
        guard link.save() else { return finish(failed: "The link could not be saved.") }
        finish(failed: nil, done: "Linked with \(peerName).")
        DispatchQueue.main.async { self.onLinked?(link) }
    }
}
