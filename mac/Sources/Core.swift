import AppKit
import ApplicationServices
import SystemConfiguration

let MAGIC_TAG: Int64 = 0x4B564D53  // marks events this app posts, so its own tap lets them through

enum Mode { case idle, entering, driving, driven }

// Lets a background app hide the cursor (private WindowServer call, looked up at run time).
private func allowBackgroundCursorHide() {
    typealias DefConn = @convention(c) () -> Int32
    typealias SetProp = @convention(c) (Int32, Int32, CFString, CFTypeRef) -> Int32
    guard let h = dlopen(nil, RTLD_NOW),
          let a = dlsym(h, "_CGSDefaultConnection"), let b = dlsym(h, "CGSSetConnectionProperty") else { return }
    let conn = unsafeBitCast(a, to: DefConn.self)()
    _ = unsafeBitCast(b, to: SetProp.self)(conn, conn, "SetsCursorInBackground" as CFString, kCFBooleanTrue)
}

final class Core {
    var cfg = Config.load()
    var link: Link? = Link.load()
    var box: Box?
    let awdl = Awdl()
    let transfers = TransferList()
    let finder = Finder()
    let linker = Linker()
    var udp: UDP!
    // The peer's current address: found by Finder, or taken from any authentic packet (under L).
    var peerIP: String?
    var peerDP: UInt16?
    var lastHeard = 0.0
    var foundBy = ""
    var peerOS = ""
    var peerAddr: sockaddr_in? { locked { peerIP.flatMap { makeAddr($0, peerDP ?? link?.peerDP ?? 24800) } } }
    var peerFound: Bool { locked { lastHeard > 0 && mono() - lastHeard < 40 } }
    private var L = os_unfair_lock()
    func locked<T>(_ f: () -> T) -> T { os_unfair_lock_lock(&L); defer { os_unfair_lock_unlock(&L) }; return f() }

    // shared state (under L)
    private(set) var mode: Mode = .idle
    var peerOnline = false
    var peerName = ""
    var tapOK = false
    private var push = 0.0
    private var enterSince = 0.0, enterLast = 0.0, enterPos: UInt16 = 0, enterFailAt = -10.0
    private var lastPeer = 0.0, lastAlive = 0.0, aliveSentAt = 0.0
    private var savedPos: UInt16 = 0
    private var parkedAt = CGPoint.zero           // where this Mac's pointer was hidden
    private var heldSince = -10.0                 // when an ENTER made this Mac the holder
    private var leavePending: (pos: UInt16, since: Double, last: Double)?
    private var swallowUp: Set<UInt16> = []
    private var cursorHidden = false
    var onChange: (() -> Void)?

    // driven-side state
    private var vpos = CGPoint.zero
    private var drivenEdge: Edge = .top
    private var buttons: Set<UInt8> = []
    private var keysDown: Set<UInt16> = []   // mapped HID usages
    private var flags: UInt64 = 0           // device + generic modifier bits
    private var scrollAcc = (x: 0, y: 0)
    private var lastClick = (t: 0.0, b: UInt8(0), p: CGPoint.zero, n: Int64(0))
    private var rawDown: Set<UInt16> = []    // the peer's keys as it sent them (HID usages), before mapping
    private var switcher: UInt16?            // mapped modifier held for the peer's Alt+Tab / Control+Tab
    private var repeatKey: UInt16?
    private var repeatTimer: DispatchSourceTimer?
    private let src = CGEventSource(stateID: .hidSystemState)

    // stats
    private var sendLat: [Double] = []
    private var rtt: [Double] = []

    var tap: CFMachPort?
    private let tick = DispatchQueue(label: "tick", qos: .userInteractive)
    private var tickTimer: DispatchSourceTimer?
    private var displays: [CGRect] = []

    init() {
        allowBackgroundCursorHide()
        refreshDisplays()
        box = Box(link: link)
        if let l = link { setOtherName(l.peerName); peerDP = l.peerDP }
        udp = UDP(port: portBase)
        if udp == nil { log("cannot bind UDP \(portBase)") }
        finder.core = self
        linker.onLinked = { [weak self] l in self?.linked(l) }
    }

    /// A packet or answer came from the peer at ip.
    func heard(ip: String, by: String?) {
        let changed = locked { () -> Bool in
            guard link != nil else { return false }
            let c = peerIP != ip; peerIP = ip; lastHeard = mono(); if let b = by { foundBy = b }; return c
        }
        if changed { log("\(otherComputer()) is at \(ip) (found by \(by ?? "its own packets"))"); DispatchQueue.main.async { self.onChange?() } }
    }

    /// A new link was made (Linker): use it at once.
    func linked(_ l: Link) {
        locked { link = l; box = Box(link: l); peerIP = nil; peerDP = l.peerDP; lastHeard = 0; foundBy = ""; peerOnline = false }
        setOtherName(l.peerName)
        finder.setVisible(false); finder.setLooking(false)
        if let u = udp { finder.startPresence(udp4: u.fd) }
        finder.presenceNow()
        log("linked with \(l.peerName)")
        onChange?()
    }

    /// Forget the link here; tell the peer first when it answers.
    func forget(tellPeer: Bool) {
        guard let l = locked({ link }) else { return }
        stopDriving(sendLeave: true, pos: nil, why: "link forgotten"); endDriven(why: "link forgotten")
        if tellPeer, peerFound { _ = request([0x29]) }
        Link.forget()
        locked { link = nil; box = nil; peerIP = nil; lastHeard = 0; peerOnline = false; foundBy = "" }
        setOtherName("")
        log("forgot \(l.peerName)")
        DispatchQueue.main.async { self.onChange?() }
    }

    // MARK: start

    func start() {
        if udp != nil {
            let tcpOK = tcpServe(port: portBase + 1, box: { [weak self] in self?.locked { self?.box } }) { [weak self] req, fd in self?.handleTCP(req, fd) }
            if !tcpOK { log("cannot bind TCP \(portBase + 1)") }
            let t = Thread { [weak self] in self?.recvLoop() }
            t.qualityOfService = .userInteractive; t.start()
            if link != nil { finder.startPresence(udp4: udp.fd) }
        }
        let tt = DispatchSource.makeTimerSource(queue: tick)
        tt.schedule(deadline: .now(), repeating: .milliseconds(50), leeway: .milliseconds(5))
        tt.setEventHandler { [weak self] in self?.onTick() }
        tt.resume(); tickTimer = tt
        Thread.detachNewThread { [weak self] in
            while let s = self { s.presence(); Thread.sleep(forTimeInterval: 3) }
        }
    }


    func refreshDisplays() {
        var n: UInt32 = 0
        CGGetActiveDisplayList(0, nil, &n)
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(n))
        CGGetActiveDisplayList(n, &ids, &n)
        let d = ids.prefix(Int(n)).map { CGDisplayBounds($0) }
        locked { displays = d }
    }

    // MARK: sending

    func send(_ plain: [UInt8], to addr: sockaddr_in? = nil) {
        guard let box = locked({ box }), let a = addr ?? peerAddr else { return }
        udp.send(box.seal(plain), to: a)
    }
    private func ack(_ t: UInt8, _ a: sockaddr_in) { send([0x02, t], to: a) }

    // MARK: geometry

    private var union: CGRect { displays.reduce(CGRect.null) { $0.union($1) } }
    private func onAny(_ p: CGPoint) -> Bool { displays.contains { $0.contains(p) } }

    /// True if p sits on the outer boundary of the whole screen space on `edge`.
    private func atOuterEdge(_ p: CGPoint, _ e: Edge) -> Bool {
        guard let d = displays.first(where: { $0.insetBy(dx: -0.5, dy: -0.5).contains(p) }) else { return false }
        var probe = p
        switch e {
        case .top: guard p.y <= d.minY + 0.5 else { return false }; probe.y = d.minY - 2
        case .bottom: guard p.y >= d.maxY - 1.5 else { return false }; probe.y = d.maxY + 2
        case .left: guard p.x <= d.minX + 0.5 else { return false }; probe.x = d.minX - 2
        case .right: guard p.x >= d.maxX - 1.5 else { return false }; probe.x = d.maxX + 2
        }
        return !onAny(probe)
    }
    private func pushAmount(_ dx: Double, _ dy: Double, _ e: Edge) -> Double {
        switch e { case .top: return -dy; case .bottom: return dy; case .left: return -dx; case .right: return dx }
    }
    private func posAlong(_ p: CGPoint, _ e: Edge) -> UInt16 {
        let u = union
        let f = (e == .top || e == .bottom) ? (p.x - u.minX) / max(u.width - 1, 1) : (p.y - u.minY) / max(u.height - 1, 1)
        return UInt16(max(0, min(65535, (f * 65535).rounded())))
    }
    /// Point just inside the outer boundary on `e` at `pos`.
    private func pointAt(_ e: Edge, _ pos: UInt16, inset: CGFloat) -> CGPoint {
        let u = union, f = CGFloat(pos) / 65535
        if e == .top || e == .bottom {
            let x = u.minX + f * (u.width - 1)
            let cov = displays.filter { $0.minX <= x && x < $0.maxX }
            if e == .top, let d = cov.min(by: { $0.minY < $1.minY }) { return CGPoint(x: x, y: d.minY + inset) }
            if e == .bottom, let d = cov.max(by: { $0.maxY < $1.maxY }) { return CGPoint(x: x, y: d.maxY - 1 - inset) }
            return clamp(CGPoint(x: x, y: e == .top ? u.minY : u.maxY - 1))
        } else {
            let y = u.minY + f * (u.height - 1)
            let cov = displays.filter { $0.minY <= y && y < $0.maxY }
            if e == .left, let d = cov.min(by: { $0.minX < $1.minX }) { return CGPoint(x: d.minX + inset, y: y) }
            if e == .right, let d = cov.max(by: { $0.maxX < $1.maxX }) { return CGPoint(x: d.maxX - 1 - inset, y: y) }
            return clamp(CGPoint(x: e == .left ? u.minX : u.maxX - 1, y: y))
        }
    }
    private func clamp(_ p: CGPoint) -> CGPoint {
        if onAny(p) { return p }
        var best = p, bd = CGFloat.infinity
        for d in displays {
            let q = CGPoint(x: min(max(p.x, d.minX), d.maxX - 1), y: min(max(p.y, d.minY), d.maxY - 1))
            let dist = hypot(q.x - p.x, q.y - p.y)
            if dist < bd { bd = dist; best = q }
        }
        return best
    }

    // MARK: event tap (driver side)

    func installTap() -> Bool {
        if tap != nil { return true }
        if inputDisabled { return false }
        let types: [CGEventType] = [.mouseMoved, .leftMouseDown, .leftMouseUp, .rightMouseDown, .rightMouseUp,
                                    .otherMouseDown, .otherMouseUp, .leftMouseDragged, .rightMouseDragged,
                                    .otherMouseDragged, .scrollWheel, .keyDown, .keyUp, .flagsChanged]
        // NX_SYSDEFINED (14): media, volume and brightness keys. The HID-level tap sees keys before macOS
        // acts on its own shortcuts (⌘Tab, ⌘Space, Mission Control...), so they can go to the other computer.
        let mask = types.reduce(CGEventMask(1 << 14)) { $0 | (1 << $1.rawValue) }
        guard let t = CGEvent.tapCreate(tap: .cghidEventTap, place: .headInsertEventTap, options: .defaultTap,
                                        eventsOfInterest: mask, callback: tapCallback,
                                        userInfo: Unmanaged.passUnretained(self).toOpaque()) else { return false }
        tap = t
        let th = Thread {
            let s = CFMachPortCreateRunLoopSource(nil, t, 0)
            CFRunLoopAddSource(CFRunLoopGetCurrent(), s, .commonModes)
            CGEvent.tapEnable(tap: t, enable: true)
            CFRunLoopRun()
        }
        th.qualityOfService = .userInteractive; th.name = "tap"; th.start()
        locked { tapOK = true }
        log("event tap installed")
        return true
    }

    func onTap(_ type: CGEventType, _ ev: CGEvent) -> Unmanaged<CGEvent>? {
        if type == .tapDisabledByTimeout || type == .tapDisabledByUserInput {
            if let t = tap { CGEvent.tapEnable(tap: t, enable: true) }
            log("tap re-enabled after \(type == .tapDisabledByTimeout ? "timeout" : "user input")")
            return Unmanaged.passUnretained(ev)
        }
        if ev.getIntegerValueField(.eventSourceUserData) == MAGIC_TAG { return Unmanaged.passUnretained(ev) }
        let pass = Unmanaged.passUnretained(ev)
        os_unfair_lock_lock(&L)
        let m = mode, paused = cfg.paused
        os_unfair_lock_unlock(&L)
        if paused { return pass }
        // The pointer is on the other computer: every local key and mouse event goes there.
        if m == .driving { return forward(type, ev) }
        // The pointer is here (idle, or driven by the other computer's input too): local input acts
        // here as usual, and pushing past the edge hands the pointer over.

        // idle / entering: shortcuts and edge detection
        switch type {
        case .keyDown, .keyUp:
            let vk = UInt16(ev.getIntegerValueField(.keyboardEventKeycode))
            if debugKeys && type == .keyDown && ev.flags.contains(.maskCommand) { log("debug: Command+key \(vk) flags \(String(ev.flags.rawValue, radix: 16)) pending \(locked { pendingFiles })") }
            if type == .keyUp, locked({ swallowUp.remove(vk) }) != nil { return nil }
            if type == .keyDown, let which = dedicated(vk, ev.flags) {
                locked { _ = swallowUp.insert(vk) }
                if ev.getIntegerValueField(.keyboardEventAutorepeat) == 0 {
                    DispatchQueue.global(qos: .userInitiated).async { which == 8 ? self.copyHereToThere(remote: false) : self.pasteThereToHere(remote: false) }
                }
                return nil
            }
            if type == .keyDown, vk == 9, isCmdOnly(ev.flags), holdPasteIfWaiting() {
                locked { _ = swallowUp.insert(vk) }
                return nil
            }
            return pass
        case .mouseMoved:
            guard locked({ box != nil && peerIP != nil && cfg.macDrivesPeer }) else { return pass }
            if CGEventSource.buttonState(.combinedSessionState, button: .left) ||
               CGEventSource.buttonState(.combinedSessionState, button: .right) ||
               CGEventSource.buttonState(.combinedSessionState, button: .center) { return pass }
            let p = ev.location
            let dx = Double(ev.getIntegerValueField(.mouseEventDeltaX)), dy = Double(ev.getIntegerValueField(.mouseEventDeltaY))
            os_unfair_lock_lock(&L)
            let e = cfg.edge
            let now = mono()
            if atOuterEdge(p, e), pushAmount(dx, dy, e) > 0 {
                push += pushAmount(dx, dy, e)
                if push >= cfg.push, m == .idle || m == .driven, now - enterFailAt > 1.0 {
                    os_unfair_lock_unlock(&L)
                    crossOut(at: p)
                    return pass
                }
            } else if !atOuterEdge(p, e) { push = 0 }
            os_unfair_lock_unlock(&L)
            return pass
        default:
            return pass
        }
    }

    /// The pointer was pushed past the edge at p: ask the other computer to take it (ENTER).
    func crossOut(at p: CGPoint) {
        guard locked({ !cfg.paused && cfg.macDrivesPeer && peerIP != nil && mono() - enterFailAt > 1.0 }) else { return }
        if locked({ mode == .driven }) { endDriven(why: "the pointer crossed to the other computer") }
        let sm = locked { () -> Bool? in
            guard mode == .idle else { return nil }
            let now = mono(); mode = .entering; enterSince = now; enterLast = now
            enterPos = posAlong(p, cfg.edge); savedPos = enterPos
            return cfg.smooth
        }
        guard let sm else { return }
        sendEnter()
        if sm { awdl.sync(want: true, force: true) }
    }

    private func isCmdOnly(_ f: CGEventFlags) -> Bool {
        f.contains(.maskCommand) && !f.contains(.maskShift) && !f.contains(.maskControl) && !f.contains(.maskAlternate)
    }
    /// The dedicated copy (8) / paste (9) shortcut, when that mode is chosen.
    private func dedicated(_ vk: UInt16, _ f: CGEventFlags) -> UInt16? {
        let (on, c, p) = locked { (!cfg.usualShortcuts && cfg.clipboard && !cfg.paused && peerOnline, cfg.copyKey, cfg.pasteKey) }
        guard on else { return nil }
        if c.matches(vk: vk, flags: f.rawValue) { return 8 }
        if p.matches(vk: vk, flags: f.rawValue) { return 9 }
        return nil
    }

    private func sendEnter() {
        let (e, p) = locked { (cfg.edge.opposite, enterPos) }
        var b: [UInt8] = [0x01, e.wire]; b.u16(p)
        send(b)
    }

    /// While driving: forward everything, swallow locally.
    private func forward(_ type: CGEventType, _ ev: CGEvent) -> Unmanaged<CGEvent>? {
        let t0 = mono()
        var pkt: [UInt8] = []
        switch type {
        case .mouseMoved, .leftMouseDragged, .rightMouseDragged, .otherMouseDragged:
            pkt = [0x10]; pkt.i16(Int(ev.getIntegerValueField(.mouseEventDeltaX))); pkt.i16(Int(ev.getIntegerValueField(.mouseEventDeltaY)))
        case .leftMouseDown, .leftMouseUp: pkt = [0x11, 1, type == .leftMouseDown ? 1 : 0]
        case .rightMouseDown, .rightMouseUp: pkt = [0x11, 2, type == .rightMouseDown ? 1 : 0]
        case .otherMouseDown, .otherMouseUp:
            let n = ev.getIntegerValueField(.mouseEventButtonNumber)
            let b: UInt8 = n == 3 ? 4 : n == 4 ? 5 : 3
            pkt = [0x11, b, type == .otherMouseDown ? 1 : 0]
        case .scrollWheel:
            var dx: Int, dy: Int
            if ev.getIntegerValueField(.scrollWheelEventIsContinuous) != 0 {
                dy = -Int(ev.getDoubleValueField(.scrollWheelEventPointDeltaAxis1) * 4)
                dx = -Int(ev.getDoubleValueField(.scrollWheelEventPointDeltaAxis2) * 4)
            } else {
                dy = -Int(ev.getIntegerValueField(.scrollWheelEventDeltaAxis1)) * 120
                dx = -Int(ev.getIntegerValueField(.scrollWheelEventDeltaAxis2)) * 120
            }
            if dx == 0 && dy == 0 { return nil }
            pkt = [0x12]; pkt.i16(dx); pkt.i16(dy)
        case .keyDown, .keyUp:
            let vk = UInt16(ev.getIntegerValueField(.keyboardEventKeycode))
            if type == .keyUp, locked({ swallowUp.remove(vk) }) != nil { return nil }
            if type == .keyDown, let which = dedicated(vk, ev.flags) {
                locked { _ = swallowUp.insert(vk) }
                if ev.getIntegerValueField(.keyboardEventAutorepeat) == 0 {
                    let held = heldMods(ev.flags)
                    DispatchQueue.global(qos: .userInitiated).async {
                        which == 8 ? self.copyHereToThere(remote: true, shifts: held) : self.pasteThereToHere(remote: true, shifts: held)
                    }
                }
                return nil
            }
            if type == .keyDown, ev.getIntegerValueField(.keyboardEventAutorepeat) != 0 { return nil }
            guard let u = vkToHid[vk] else { return nil }
            pkt = [0x13]; pkt.u16(u); pkt.append(type == .keyDown ? 1 : 0)
        case .flagsChanged:
            let vk = UInt16(ev.getIntegerValueField(.keyboardEventKeycode))
            let raw = ev.flags.rawValue
            if raw & 0x2 != 0 && raw & 0x4 != 0 {   // both Shift keys: take control back
                stopDriving(sendLeave: true, pos: nil, why: "both Shift keys")
                return nil
            }
            if vk == 63 { return nil }               // fn / Globe: no equivalent there; swallowed
            if vk == 57 { // Caps Lock toggles: send a press
                var a: [UInt8] = [0x13]; a.u16(0x39); a.append(1); a += [0x13, 0x00, 0x39, 0]
                pkt = a
            } else if let m = modByVk[vk] {
                pkt = [0x13]; pkt.u16(m.hid); pkt.append(raw & m.dev != 0 ? 1 : 0)
            } else { return nil }
        default:
            // Media keys: volume, mute and playback go to the computer the pointer is on; brightness and
            // keyboard light stay on this Mac (they belong to its screen and keyboard).
            if type.rawValue == 14, let ne = NSEvent(cgEvent: ev), ne.subtype.rawValue == 8 {
                let code = (ne.data1 & 0xFFFF0000) >> 16, down = ((ne.data1 & 0xFF00) >> 8) == 0xA
                let map: [Int: UInt16] = [0: 0xED, 1: 0xEE, 7: 0xEF, 16: 0xE8, 17: 0xEB, 18: 0xEA, 19: 0xEB, 20: 0xEA]
                guard let u = map[code] else { return Unmanaged.passUnretained(ev) }
                if ne.data1 & 0x1 != 0 && down { return nil }   // auto-repeat
                pkt = [0x13]; pkt.u16(u); pkt.append(down ? 1 : 0)
            } else { return nil }
        }
        send(pkt)
        let t1 = mono()
        let evAge = Double(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) &- ev.timestamp) / 1e9
        locked { if sendLat.count < 100_000 { sendLat.append(evAge >= 0 && evAge < 1 ? evAge : t1 - t0) } }
        return nil
    }

    /// Modifier keys held now other than Command (they are lifted around a synthetic Command+key).
    private func heldMods(_ f: CGEventFlags) -> [UInt16] {
        modBits.filter { f.rawValue & $0.dev != 0 && $0.hid != 0xE3 && $0.hid != 0xE7 }.map { $0.hid }
    }

    /// The pointer is now on the other computer: hide it here, park it where it is, forward all input.
    private func startDriving() {
        let here = currentPointer()
        locked {
            mode = .driving; lastPeer = mono(); lastAlive = 0; push = 0; parkedAt = here
            sendLat.removeAll(); rtt.removeAll()
        }
        if !inputDisabled { CGAssociateMouseAndMouseCursorPosition(0) }
        DispatchQueue.main.async {
            if !self.cursorHidden { if !inputDisabled { CGDisplayHideCursor(CGMainDisplayID()) }; self.cursorHidden = true }
            self.onChange?()
        }
        log("the pointer is on \(otherComputer()); this Mac's pointer is hidden")
    }
    /// Where this Mac's pointer is now (test copies keep their own idea of it).
    private func currentPointer() -> CGPoint {
        if inputDisabled { return locked { vpos } }
        return CGEvent(source: nil)?.location ?? locked { vpos }
    }
    /// Test copies: whether the pointer here is hidden.
    var pointerHidden: Bool { onMain { cursorHidden } }

    /// The pointer comes back to this Mac: shown again where it was parked (or, with entry, where it
    /// comes in). sendLeave: this Mac takes it back (LEAVE), the other computer becomes the remote.
    func stopDriving(sendLeave: Bool, pos: UInt16?, why: String, entry: Edge? = nil) {
        let (was, target) = locked { () -> (Mode, CGPoint) in
            let w = mode
            if w == .driving { mode = .idle; push = 0 }
            if sendLeave && w == .driving { let now = mono(); leavePending = (0, now, now) }
            let t = pos.map { pointAt(entry ?? cfg.edge, $0, inset: 1) } ?? clamp(parkedAt)
            if w == .driving { vpos = t }
            return (w, t)
        }
        guard was == .driving else { return }
        if sendLeave { var b: [UInt8] = [0x03]; b.u16(0); send(b) }
        if !inputDisabled { CGWarpMouseCursorPosition(target); CGAssociateMouseAndMouseCursorPosition(1) }
        DispatchQueue.main.async {
            if self.cursorHidden { if !inputDisabled { CGDisplayShowCursor(CGMainDisplayID()) }; self.cursorHidden = false }
            self.onChange?()
        }
        logStats(why)
        awdl.sync(want: false, force: true)
    }

    private func logStats(_ why: String) {
        let (s, r) = locked { (sendLat.sorted(), rtt.sorted()) }
        func ms(_ a: [Double], _ q: Double) -> String { a.isEmpty ? "-" : String(format: "%.3f", a[min(a.count - 1, Int(Double(a.count) * q))] * 1000) }
        log("control back (\(why)); event->datagram ms median \(ms(s, 0.5)) p99 \(ms(s, 0.99)) worst \(ms(s, 1)) n=\(s.count); ALIVE round trip ms median \(ms(r, 0.5)) worst \(ms(r, 1)) n=\(r.count)")
    }

    // MARK: tick: retries and timeouts

    private func onTick() {
        let now = mono()
        var doEnter = false, doAlive = false, timeout = false, drivenTimeout = false
        var leaveResend: UInt16?
        locked {
            switch mode {
            case .entering:
                if now - enterSince > 1.0 { mode = .idle; enterFailAt = now; push = 0 }
                else if now - enterLast >= 0.2 { enterLast = now; doEnter = true }
            case .driving:
                if now - lastPeer > 2.0 { timeout = true }
                else if now - lastAlive >= 0.5 { lastAlive = now; aliveSentAt = now; doAlive = true }
            case .driven:
                if now - lastPeer > 2.0 { drivenTimeout = true }
            case .idle: break
            }
            if let lp = leavePending {
                if now - lp.since > 1.5 { leavePending = nil }
                else if now - lp.last >= 0.2 { leavePending = (lp.pos, lp.since, now); leaveResend = lp.pos }
            }
        }
        let (sm, active) = locked { (cfg.smooth, mode != .idle) }
        awdl.sync(want: sm && active)
        if doEnter { sendEnter() }
        if doAlive { send([0x1F]) }
        if timeout { stopDriving(sendLeave: true, pos: nil, why: "the other computer stopped answering") }
        if drivenTimeout { endDriven(why: "the other computer went quiet") }
        if let p = leaveResend { var b: [UInt8] = [0x03]; b.u16(p); send(b) }
    }

    // MARK: receiving

    private func recvLoop() {
        while true {
            guard let (pkt, from) = udp.recv() else { continue }
            if pkt.count > 7, Array(pkt[0..<6]) == MAGIC {
                var ss = sockaddr_storage(); withUnsafeMutablePointer(to: &ss) { $0.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee = from } }
                finder.handlePresent(pkt, ss); continue
            }
            guard let box = locked({ box }), let plain = box.open(pkt, replayCheck: true) else { continue }
            locked { lastPeer = mono() }
            heard(ip: ipString(from.sin_addr), by: nil)
            var r = Reader(plain)
            while let t = r.u8() {
                switch t {
                case 0x01:
                    guard let e = r.u8(), let pos = r.u16() else { break }
                    onEnter(Edge(wire: e) ?? cfg.edge, pos, from)
                case 0x02:
                    guard let a = r.u8() else { break }
                    onAck(a)
                case 0x03:
                    guard let pos = r.u16() else { break }
                    onLeave(pos, from)
                case 0x10:
                    guard let dx = r.i16(), let dy = r.i16() else { break }
                    if holding() { drivenMove(Double(dx), Double(dy)) }
                case 0x11:
                    guard let b = r.u8(), let d = r.u8() else { break }
                    if holding() { drivenButton(b, d != 0) }
                case 0x12:
                    guard let dx = r.i16(), let dy = r.i16() else { break }
                    if holding() { drivenScroll(dx, dy) }
                case 0x13:
                    guard let u = r.u16(), let d = r.u8() else { break }
                    if holding() { drivenKey(u, d != 0) }
                case 0x1F:
                    ack(0x1F, from)
                default:
                    r.i = r.b.count  // unknown type: drop the rest
                }
            }
        }
    }

    /// This Mac holds the pointer and may apply the other computer's input: switches idle to driven,
    /// starting from where the pointer is now.
    private func holding() -> Bool {
        let here = currentPointer()
        let became = locked { () -> Bool? in
            guard !cfg.paused && cfg.peerDrivesMac else { return nil }
            if mode == .driven { return false }
            guard mode == .idle else { return nil }
            mode = .driven; drivenEdge = cfg.edge; lastPeer = mono(); push = 0; vpos = here
            buttons = []; keysDown = []; rawDown = []; switcher = nil; flags = 0; scrollAcc = (0, 0)
            return true
        }
        guard let became else { return false }
        if became { DispatchQueue.main.async { self.onChange?() }; if locked({ cfg.smooth }) { awdl.sync(want: true, force: true) }; log("\(otherComputer())'s keyboard and mouse act on this Mac") }
        return true
    }

    private func onAck(_ a: UInt8) {
        switch a {
        case 0x01: if locked({ mode == .entering }) { startDriving() }
        case 0x03: locked { leavePending = nil }
        case 0x1F: locked { if aliveSentAt > 0 { rtt.append(mono() - aliveSentAt); aliveSentAt = 0 } }
        default: break
        }
    }

    /// The other computer took the pointer back to its screen: this Mac becomes the remote.
    private func onLeave(_ pos: UInt16, _ from: sockaddr_in) {
        ack(0x03, from)
        let (m, ok) = locked { (mode, !cfg.paused && cfg.macDrivesPeer) }
        guard m != .driving, ok else { return }
        if m == .driven { endDriven(why: "the other computer took the pointer back") }
        locked { if mode == .entering { mode = .idle } }
        startDriving()
    }

    /// The other computer hands the pointer to this Mac.
    private func onEnter(_ e: Edge, _ pos: UInt16, _ from: sockaddr_in) {
        if locked({ cfg.paused || !cfg.peerDrivesMac }) { return }
        let m = locked { mode }
        if m == .entering {   // both handed over at once: the lower id keeps its handover
            if let l = locked({ link }), myID.lexicographicallyPrecedes(l.peerID) { return }
            locked { mode = .idle }
        }
        ack(0x01, from)
        if (m == .driven || m == .idle) && mono() - locked({ heldSince }) < 1.5 { return }   // a repeat of the ENTER that gave it
        if m == .driving { stopDriving(sendLeave: false, pos: pos, why: "the pointer came back", entry: e) }
        let p = locked { () -> CGPoint in
            mode = .driven; drivenEdge = e; lastPeer = mono(); push = 0; heldSince = mono()
            buttons = []; keysDown = []; rawDown = []; switcher = nil; flags = 0; scrollAcc = (0, 0)
            vpos = pointAt(e, pos, inset: 1)
            return vpos
        }
        if m != .driving, !inputDisabled { CGWarpMouseCursorPosition(p) }
        post(mouse: .mouseMoved, at: p, dx: 0, dy: 0)
        DispatchQueue.main.async { self.onChange?() }
        if locked({ cfg.smooth }) { awdl.sync(want: true, force: true) }
        log("the pointer is on this Mac (from \(otherComputer()))")
    }

    // MARK: driven side (inject)

    private func mk(_ e: CGEvent?) -> CGEvent? {
        if inputDisabled { return nil }   // test copies never type or point on this Mac
        e?.setIntegerValueField(.eventSourceUserData, value: MAGIC_TAG); return e
    }

    private func post(mouse type: CGEventType, at p: CGPoint, dx: Double, dy: Double, button: CGMouseButton = .left, clicks: Int64 = 0, num: Int64 = -1) {
        guard let e = mk(CGEvent(mouseEventSource: src, mouseType: type, mouseCursorPosition: p, mouseButton: button)) else { return }
        e.setIntegerValueField(.mouseEventDeltaX, value: Int64(dx))
        e.setIntegerValueField(.mouseEventDeltaY, value: Int64(dy))
        if clicks > 0 { e.setIntegerValueField(.mouseEventClickState, value: clicks) }
        if num >= 0 { e.setIntegerValueField(.mouseEventButtonNumber, value: num) }
        e.flags = CGEventFlags(rawValue: locked { flags })
        e.post(tap: .cghidEventTap)
    }

    private func drivenMove(_ dx0: Double, _ dy0: Double) {
        var leavePos: UInt16?
        let here = currentPointer()   // from wherever the pointer is now, whichever mouse moved it last
        let (p, type, btn, dx, dy) = locked { () -> (CGPoint, CGEventType, CGMouseButton, Double, Double) in
            let dx = dx0 * cfg.speed, dy = dy0 * cfg.speed
            vpos = clamp(CGPoint(x: here.x + dx, y: here.y + dy))
            let e = cfg.edge
            if buttons.isEmpty, atOuterEdge(vpos, e), pushAmount(dx, dy, e) > 0 {
                push += pushAmount(dx, dy, e)
                if push >= cfg.push { leavePos = posAlong(vpos, e) }
            } else if !atOuterEdge(vpos, e) { push = 0 }
            let t: CGEventType, b: CGMouseButton
            if buttons.contains(1) { t = .leftMouseDragged; b = .left }
            else if buttons.contains(2) { t = .rightMouseDragged; b = .right }
            else if !buttons.isEmpty { t = .otherMouseDragged; b = .center }
            else { t = .mouseMoved; b = .left }
            return (vpos, t, b, dx, dy)
        }
        post(mouse: type, at: p, dx: dx, dy: dy, button: btn)
        if leavePos != nil { crossOut(at: p) }   // pushed past the edge by the other computer's mouse: hand over
    }

    private func drivenButton(_ b: UInt8, _ down: Bool) {
        let (p, clicks) = locked { () -> (CGPoint, Int64) in
            if down { buttons.insert(b) } else { buttons.remove(b) }
            var n = lastClick.n
            if down {
                let now = mono()
                if b == lastClick.b, now - lastClick.t < NSEvent.doubleClickInterval,
                   hypot(vpos.x - lastClick.p.x, vpos.y - lastClick.p.y) < 5 { n += 1 } else { n = 1 }
                lastClick = (now, b, vpos, n)
            }
            return (vpos, max(n, 1))
        }
        switch b {
        case 1: post(mouse: down ? .leftMouseDown : .leftMouseUp, at: p, dx: 0, dy: 0, button: .left, clicks: clicks)
        case 2: post(mouse: down ? .rightMouseDown : .rightMouseUp, at: p, dx: 0, dy: 0, button: .right, clicks: clicks)
        default:
            let num: Int64 = b == 4 ? 3 : b == 5 ? 4 : 2
            post(mouse: down ? .otherMouseDown : .otherMouseUp, at: p, dx: 0, dy: 0, button: .center, clicks: clicks, num: num)
        }
    }

    private func drivenScroll(_ dx: Int, _ dy: Int) {
        let (lx, ly) = locked { () -> (Int32, Int32) in
            scrollAcc.x += dx; scrollAcc.y += dy
            let lx = scrollAcc.x / 120, ly = scrollAcc.y / 120
            scrollAcc.x -= lx * 120; scrollAcc.y -= ly * 120
            return (Int32(lx), Int32(ly))
        }
        if lx == 0 && ly == 0 { return }
        guard let e = mk(CGEvent(scrollWheelEvent2Source: src, units: .line, wheelCount: 2, wheel1: -ly, wheel2: -lx, wheel3: 0)) else { return }
        e.flags = CGEventFlags(rawValue: locked { flags })
        e.post(tap: .cghidEventTap)
    }

    /// Linux Control acts as Command and Super as Control (or, with "Command acts as Super", Super as
    /// Command and Control as Control). Option = Alt.
    private func mapped(_ u: UInt16) -> UInt16 {
        let sup = locked { cfg.commandSuper }
        let m: [UInt16: UInt16] = sup ? [0xE3: 0xE3, 0xE7: 0xE7, 0xE0: 0xE0, 0xE4: 0xE4] : [0xE0: 0xE3, 0xE4: 0xE7, 0xE3: 0xE0, 0xE7: 0xE4]
        return m[u] ?? u
    }
    private func postFlags(_ vk: UInt16, _ down: Bool, _ f: UInt64) {
        guard let e = mk(CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: down)) else { return }
        e.type = .flagsChanged; e.flags = CGEventFlags(rawValue: f); e.post(tap: .cghidEventTap)
    }
    private func setMod(_ hid: UInt16, _ down: Bool) -> UInt64 {
        guard let m = modByHid[hid] else { return locked { flags } }
        return locked { () -> UInt64 in
            if down { flags |= m.dev; keysDown.insert(hid) } else { flags &= ~m.dev; keysDown.remove(hid) }
            var g: UInt64 = 0
            for mb in modBits where flags & mb.dev != 0 { g |= mb.generic }
            flags = (flags & 0xFFFF) | g
            return flags
        }
    }
    /// The app switcher and tab switching keep their meaning: the peer's Alt+Tab (repeated, with Shift)
    /// is ⌘Tab with ⌘ held until Alt is released; its Control+Tab stays ⌃Tab.
    private func switcherKey(_ u0: UInt16, _ down: Bool) -> Bool {
        let (raw, sw) = locked { () -> (Set<UInt16>, UInt16?) in if down { rawDown.insert(u0) } else { rawDown.remove(u0) }; return (rawDown, switcher) }
        let alt = raw.contains(0xE2) || raw.contains(0xE6), ctl = raw.contains(0xE0) || raw.contains(0xE4)
        if u0 == 0x2B && down && sw == nil {
            if alt {          // lift Option, hold Command
                for h in [UInt16(0xE2), 0xE6] where locked({ keysDown.contains(h) }) { let f = setMod(h, false); postFlags(modByHid[h]!.vk, false, f) }
                let f = setMod(0xE3, true); postFlags(55, true, f); locked { switcher = 0xE3 }
            } else if ctl && mapped(0xE0) != 0xE0 {   // Control is Command here: make it Control for Tab
                for h in [UInt16(0xE3), 0xE7] where locked({ keysDown.contains(h) }) { let f = setMod(h, false); postFlags(modByHid[h]!.vk, false, f) }
                let f = setMod(0xE0, true); postFlags(59, true, f); locked { switcher = 0xE0 }
            }
            return false
        }
        if let s = sw {
            let ends = s == 0xE3 ? (u0 == 0xE2 || u0 == 0xE6) : (u0 == 0xE0 || u0 == 0xE4)
            if ends {
                if !(s == 0xE3 ? alt : ctl) { let f = setMod(s, false); postFlags(modByHid[s]!.vk, false, f); locked { switcher = nil } }
                return true
            }
        }
        return false
    }
    private func mediaKey(_ u: UInt16, _ down: Bool) -> Bool {
        let map: [UInt16: Int] = [0xED: 0, 0xEE: 1, 0xEF: 7, 0x80: 0, 0x81: 1, 0x7F: 7, 0xE8: 16, 0xEB: 17, 0xEA: 18]
        guard let t = map[u] else { return false }
        let e = NSEvent.otherEvent(with: .systemDefined, location: .zero, modifierFlags: NSEvent.ModifierFlags(rawValue: down ? 0xa00 : 0xb00),
                                   timestamp: 0, windowNumber: 0, context: nil, subtype: 8, data1: (t << 16) | ((down ? 0xa : 0xb) << 8), data2: -1)
        if !inputDisabled, let c = e?.cgEvent { c.setIntegerValueField(.eventSourceUserData, value: MAGIC_TAG); c.post(tap: .cghidEventTap) }
        return true
    }

    private func drivenKey(_ u0: UInt16, _ down: Bool) {
        if switcherKey(u0, down) { return }
        if mediaKey(u0, down) { return }
        let u = mapped(u0)
        if let m = modByHid[u] {
            let f = locked { () -> UInt64 in
                if down { flags |= m.dev; keysDown.insert(u) } else { flags &= ~m.dev; keysDown.remove(u) }
                // recompute generic bits from device bits
                var g: UInt64 = 0
                for mb in modBits where flags & mb.dev != 0 { g |= mb.generic }
                flags = (flags & 0xFFFF) | g
                return flags
            }
            guard let e = mk(CGEvent(keyboardEventSource: src, virtualKey: m.vk, keyDown: down)) else { return }
            e.type = .flagsChanged
            e.flags = CGEventFlags(rawValue: f)
            e.post(tap: .cghidEventTap)
            return
        }
        guard let vk = hidToVk[u] else { return }
        if u == 0x39 { // Caps Lock: toggle on press
            if down, let e = mk(CGEvent(keyboardEventSource: src, virtualKey: 57, keyDown: true)) { e.type = .flagsChanged; e.post(tap: .cghidEventTap) }
            return
        }
        let f = locked { () -> UInt64 in
            if down { keysDown.insert(u) } else { keysDown.remove(u) }
            return flags
        }
        if vk == 9, down, f & CGEventFlags.maskCommand.rawValue != 0, holdPasteIfWaiting() { locked { _ = keysDown.remove(u) }; return }
        postKey(vk, down, f, repeatFlag: false)
        if down { startRepeat(u, vk) } else if locked({ repeatKey == u }) { stopRepeat() }
    }

    private func postKey(_ vk: UInt16, _ down: Bool, _ f: UInt64, repeatFlag: Bool) {
        guard let e = mk(CGEvent(keyboardEventSource: src, virtualKey: vk, keyDown: down)) else { return }
        e.flags = CGEventFlags(rawValue: f)
        if repeatFlag { e.setIntegerValueField(.keyboardEventAutorepeat, value: 1) }
        e.post(tap: .cghidEventTap)
    }

    private func startRepeat(_ u: UInt16, _ vk: UInt16) {
        stopRepeat()
        let t = DispatchSource.makeTimerSource(queue: tick)
        let delay = max(NSEvent.keyRepeatDelay, 0.15), every = max(NSEvent.keyRepeatInterval, 0.015)
        t.schedule(deadline: .now() + delay, repeating: every, leeway: .milliseconds(2))
        t.setEventHandler { [weak self] in
            guard let s = self else { return }
            let (ok, f) = s.locked { (s.mode == .driven && s.keysDown.contains(u) && s.repeatKey == u, s.flags) }
            if ok { s.postKey(vk, true, f, repeatFlag: true) } else { s.stopRepeat() }
        }
        locked { repeatKey = u; repeatTimer = t }
        t.resume()
    }
    private func stopRepeat() {
        let t = locked { () -> DispatchSourceTimer? in let t = repeatTimer; repeatTimer = nil; repeatKey = nil; return t }
        t?.cancel()
    }

    /// Releases everything held and ends the driven session.
    func endDriven(why: String) {
        let (was, keys, btns, p) = locked { () -> (Mode, Set<UInt16>, Set<UInt8>, CGPoint) in
            let w = mode
            if w == .driven { mode = .idle; push = 0 }
            return (w, keysDown, buttons, vpos)
        }
        guard was == .driven else { return }
        stopRepeat()
        for u in keys where modByHid[u] == nil { if let vk = hidToVk[u] { postKey(vk, false, 0, repeatFlag: false) } }
        for u in keys { if let m = modByHid[u], let e = mk(CGEvent(keyboardEventSource: src, virtualKey: m.vk, keyDown: false)) {
            e.type = .flagsChanged; e.flags = []; e.post(tap: .cghidEventTap) } }
        locked { flags = 0; keysDown = []; buttons = []; rawDown = []; switcher = nil }
        for b in btns {
            switch b {
            case 1: post(mouse: .leftMouseUp, at: p, dx: 0, dy: 0, button: .left)
            case 2: post(mouse: .rightMouseUp, at: p, dx: 0, dy: 0, button: .right)
            default: post(mouse: .otherMouseUp, at: p, dx: 0, dy: 0, button: .center)
            }
        }
        DispatchQueue.main.async { self.onChange?() }
        awdl.sync(want: false, force: true)
        log("\(otherComputer()) stopped driving (\(why))")
    }

    // MARK: TCP (clipboard, presence)

    private func handleTCP(_ req: [UInt8], _ fd: Int32) -> [UInt8]? {
        guard let t = req.first else { return nil }
        var pa = sockaddr_in(); var pl = socklen_t(MemoryLayout<sockaddr_in>.size)
        if withUnsafeMutablePointer(to: &pa, { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { getpeername(fd, $0, &pl) } }) == 0, Int32(pa.sin_family) == AF_INET {
            heard(ip: ipString(pa.sin_addr), by: nil)
        }
        switch t {
        case 0x20:
            if let j = try? JSONSerialization.jsonObject(with: Data(req.dropFirst())) as? [String: Any], let n = j["name"] as? String {
                locked { peerName = n; peerOS = j["os"] as? String ?? peerOS }; setOtherName(n)
            }
            return hello()
        case 0x21:
            return [0x23] + onMain { readPasteboard() }
        case 0x22:
            guard req.count > 1, req[1] != 0, locked({ cfg.clipboard && !cfg.paused }) else { return [0x02, 0x22] }
            let ok = onMain { () -> Bool in let r = writePasteboard(Array(req.dropFirst())); markOwnChange(); return r }
            log("clipboard received from \(otherComputer()) (kind \(req[1]), \(req.count - 2) bytes)\(ok ? "" : " but it could not be used")")
            let t = transfers.add(Transfer(toPeer: false, what: req[1] == 1 ? "text" : req[1] == 2 ? "picture" : "files", total: UInt64(req.count - 2)))
            transfers.finish(t, ok: ok, reason: ok ? "" : "it could not be put on the clipboard")
            if req[1] == 3 { filesArrived() }
            return [0x02, 0x22]
        case 0x27:
            guard let box = locked({ box }), locked({ cfg.clipboard && !cfg.paused }) else { return nil }
            let t = transfers.incoming() ?? transfers.add(Transfer(toPeer: false, what: "files", total: 0))
            transfers.update(t, { $0.state = .running }, force: true)
            let res = receiveFiles(fd: fd, box: box, header: req) { names, got, total in
                self.transfers.update(t, { $0.what = filesSummary(names); $0.total = total; $0.done = got })
            }
            switch res {
            case .success(let urls):
                onMain { NSPasteboard.general.clearContents(); NSPasteboard.general.writeObjects(urls as [NSURL]); markOwnChange() }
                transfers.finish(t, ok: true)
                filesArrived()
                return [0x02, 0x27]
            case .failure(let e):
                transfers.finish(t, ok: false, reason: "\(e)")
                locked { pendingFiles = false; pasteDeferred = false }
                return nil
            }
        case 0x24:
            var r = Reader(Array(req.dropFirst())); let total = r.u64() ?? 0
            locked { pendingFiles = total > 0 && cfg.clipboard && !cfg.paused; pendingSince = mono() }
            if total == 0 {
                if let t = transfers.incoming() { transfers.finish(t, ok: false, reason: "\(otherComputer()) stopped sending") }
                locked { pasteDeferred = false; pendingFiles = false }
            } else {
                let t = transfers.add(Transfer(toPeer: false, what: "files", total: total)); transfers.update(t, { $0.state = .waiting })
                log("files copied on \(otherComputer()) are on their way (\(mb(total)))")
            }
            return [0x02, 0x24]
        case 0x25:
            var r = Reader(Array(req.dropFirst())); let total = r.u64() ?? 0, limit = r.u64() ?? 0
            let t = transfers.add(Transfer(toPeer: false, what: "files", total: total))
            transfers.finish(t, ok: false, reason: "not sent: over the \(mb(limit)) limit")
            return [0x02, 0x25]
        case 0x26:
            guard let j = try? JSONSerialization.jsonObject(with: Data(req.dropFirst())) as? [String: Any] else { return [0x02, 0x26] }
            let adopted = locked { () -> Bool in let a = cfg.adopt(wire: j); if a { cfg.save() }; return a }
            if adopted { log("settings from \(otherComputer()) adopted"); applySettings() }
            else { DispatchQueue.global().async { self.sendSettings() } }   // ours are newer
            return [0x02, 0x26]
        case 0x29:
            DispatchQueue.global().async { self.forget(tellPeer: false) }
            return [0x02, 0x29]
        default:
            return nil
        }
    }
    /// Settings changed (here or by the peer): stop what is no longer allowed and redraw.
    private func applySettings() {
        let c = locked { cfg }
        if c.paused || !c.macDrivesPeer { stopDriving(sendLeave: true, pos: nil, why: "switched off") }
        if c.paused || !c.peerDrivesMac { endDriven(why: "switched off") }
        DispatchQueue.main.async { self.onChange?() }
    }
    private lazy var helloName: String = computerName()
    private func hello() -> [UInt8] {
        let d = try! JSONSerialization.data(withJSONObject: ["name": helloName, "version": 1, "os": "mac"])
        return [0x20] + Array(d)
    }
    /// One request to the peer over TCP (its clipboard port), or nil.
    func request(_ plain: [UInt8]) -> [UInt8]? {
        guard let (box, h, p) = locked({ () -> (Box, String, UInt16)? in
            guard let b = box, let ip = peerIP else { return nil }; return (b, ip, (peerDP ?? link?.peerDP ?? 24800) + 1) }) else { return nil }
        let r = tcpRequest(host: h, port: p, box: box, plain)
        if r != nil { heard(ip: h, by: nil) }
        return r
    }
    private func presence() {
        guard locked({ link != nil }) else { if locked({ peerOnline }) { locked { peerOnline = false } }; return }
        let r = locked({ peerIP }) == nil ? nil : request(hello())
        var name = "", os = ""
        if let r = r, r.first == 0x20, let j = try? JSONSerialization.jsonObject(with: Data(r.dropFirst())) as? [String: Any] {
            name = j["name"] as? String ?? ""; os = j["os"] as? String ?? "linux"
        }
        let online = r?.first == 0x20
        let changed = locked { () -> Bool in
            let c = peerOnline != online || (!name.isEmpty && name != peerName); peerOnline = online
            if !name.isEmpty { peerName = name; peerOS = os; setOtherName(name) }; return c
        }
        if changed {
            log("\(otherComputer()) \(online ? "answering" : "not answering")")
            if !online { finder.presenceNow() }
            DispatchQueue.main.async { self.onChange?() }
        }
        if online && (changed || mono() - lastSettingsSent > 60) { sendSettings() }
    }

    // MARK: clipboard shortcuts

    private func localKey(_ vk: UInt16, cmd: Bool) {
        let f: UInt64 = cmd ? CGEventFlags.maskCommand.rawValue | 0x8 : 0
        postKey(vk, true, f, repeatFlag: false)
        postKey(vk, false, f, repeatFlag: false)
    }
    /// Sends a keystroke to the other computer with the held Shift keys lifted around it.
    private func remoteKey(_ u: UInt16, shifts: [UInt16]) {
        let cmdHeld = CGEventSource.flagsState(.combinedSessionState).contains(.maskCommand)
        var b: [UInt8] = []
        for s in shifts { b += [0x13]; b.u16(s); b.append(0) }
        if !cmdHeld { b += [0x13]; b.u16(0xE3); b.append(1) }
        b += [0x13]; b.u16(u); b.append(1)
        b += [0x13]; b.u16(u); b.append(0)
        if !cmdHeld { b += [0x13]; b.u16(0xE3); b.append(0) }
        for s in shifts { b += [0x13]; b.u16(s); b.append(1) }
        send(b)
    }

    /// Cmd+Shift+C. remote = the pointer is on the other computer.
    func copyHereToThere(remote: Bool, shifts: [UInt16] = []) {
        if remote {
            remoteKey(0x06, shifts: shifts)          // Command+C there (mapped there)
            Thread.sleep(forTimeInterval: 0.25)
            guard let r = request([0x21]), r.first == 0x23 else { NSSound.beep(); log("copy from the other computer failed"); return }
            if r.count > 1, r[1] != 0 { onMain { _ = writePasteboard(Array(r.dropFirst())); markOwnChange() } }
            log("copied from the other computer to this Mac (kind \(r.count > 1 ? r[1] : 0))")
        } else {
            let before = onMain { NSPasteboard.general.changeCount }
            localKey(8, cmd: true)
            let t0 = mono()
            while mono() - t0 < 0.3, onMain({ NSPasteboard.general.changeCount }) == before { Thread.sleep(forTimeInterval: 0.01) }
            let body = onMain { readPasteboard() }
            guard let r = request([0x22] + body), r.first == 0x02 else { NSSound.beep(); log("copy to the other computer failed"); return }
            log("copied from this Mac to the other computer (kind \(body[0]), \(body.count) bytes)")
        }
    }

    /// Cmd+Shift+V.
    func pasteThereToHere(remote: Bool, shifts: [UInt16] = []) {
        if remote {
            let body = onMain { readPasteboard() }
            guard let r = request([0x22] + body), r.first == 0x02 else { NSSound.beep(); log("paste on the other computer failed"); return }
            remoteKey(0x19, shifts: shifts)          // Command+V there (mapped there)
            log("pasted this Mac's clipboard there (kind \(body[0]))")
        } else {
            guard let r = request([0x21]), r.first == 0x23 else { NSSound.beep(); log("paste from the other computer failed"); return }
            if r.count > 1, r[1] != 0 { onMain { _ = writePasteboard(Array(r.dropFirst())); markOwnChange() } }
            localKey(9, cmd: true)
            log("pasted the other computer's clipboard here (kind \(r.count > 1 ? r[1] : 0))")
        }
    }

    func setEdge(_ e: Edge) { update { $0.edge = e } }

    /// Change settings, save, and tell the other computer.
    func update(_ f: (inout Config) -> Void) {
        locked { let before = cfg.wireJSON; f(&cfg); if cfg.wireJSON != before { cfg.changed = Int64(Date().timeIntervalSince1970 * 1000) }; cfg.save() }
        applySettings()
        DispatchQueue.global().async { self.sendSettings() }
    }
    private var lastSettingsSent = -100.0
    func sendSettings() {
        let body = locked { cfg.wireJSON }
        if let r = request([0x26] + body), r.first == 0x02 { locked { lastSettingsSent = mono() } }
    }

    // MARK: shared clipboard (watch the pasteboard while the pointer is on the Mac, push each change)

    private var pendingFiles = false, pendingSince = 0.0, pasteDeferred = false
    private var ownChangeCount = -1, lastChangeCount = NSPasteboard.general.changeCount  // nothing is sent at launch
    private let debugKeys = FileManager.default.fileExists(atPath: configDir.appendingPathComponent("debug-keys").path)
    private let pushQ = DispatchQueue(label: "clip-push", qos: .userInitiated)
    var lastNotice = ""
    private func mb(_ b: UInt64) -> String {
        b >= 1 << 30 ? String(format: "%.1f GB", Double(b) / Double(1 << 30)) : b >= 1 << 20 ? "\(b >> 20) MB" : b >= 1024 ? "\(b >> 10) KB" : "\(b) bytes"
    }
    func notice(_ s: String) { log(s); locked { lastNotice = s }; DispatchQueue.main.async { self.onChange?() } }
    /// Call on the main thread right after this app wrote the pasteboard (so it is not sent back).
    func markOwnChange() { ownChangeCount = NSPasteboard.general.changeCount; lastChangeCount = ownChangeCount }

    private func holdPasteIfWaiting() -> Bool {
        let held = locked { () -> Bool in
            if pendingFiles && mono() - pendingSince > 600 { pendingFiles = false }
            if pendingFiles { pasteDeferred = true; return true }
            return false
        }
        if held {
            log("paste held until the files from \(otherComputer()) are in place")
            if let t = transfers.incoming() { transfers.update(t, { $0.pasteWaiting = true }, force: true) }
        }
        return held
    }
    private func filesArrived() {
        let doPaste = locked { () -> Bool in let d = pasteDeferred; pendingFiles = false; pasteDeferred = false; return d }
        if doPaste { usleep(30_000); localKey(9, cmd: true); log("held paste done") }
    }

    /// Main-thread timer, every 0.1 s.
    func pollPasteboard() {
        let cc = NSPasteboard.general.changeCount
        guard cc != lastChangeCount else { return }
        lastChangeCount = cc
        if cc == ownChangeCount { return }
        let (on, here, big, limit) = locked { (cfg.clipboard && cfg.usualShortcuts && !cfg.paused && peerOnline && link != nil, mode != .driving, cfg.bigFiles, UInt64(cfg.fileLimitMB) << 20) }
        guard on, here else { return }
        let pb = NSPasteboard.general
        if let urls = pb.readObjects(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true]) as? [URL], !urls.isEmpty {
            pushQ.async {
                let files = listFiles(urls)
                let total = files.reduce(UInt64(0)) { $0 + $1.size }
                let t = self.transfers.add(Transfer(toPeer: true, what: filesSummary(files.map { $0.rel }), total: total))
                if !big && total > limit {
                    var b: [UInt8] = [0x25]; b.u64(total); b.u64(limit); _ = self.request(b)
                    self.transfers.finish(t, ok: false, reason: "not sent: over your \(self.mb(limit)) limit")
                    return
                }
                var p: [UInt8] = [0x24]; p.u64(total); _ = self.request(p)
                let (h, port, bx) = self.locked { (self.peerIP ?? "", (self.peerDP ?? 24800) + 1, self.box) }
                var why: String? = "\(otherComputer()) did not answer"
                if let box = bx, let fd = tcpConnect(host: h, port: port) {
                    why = sendFiles(fd: fd, box: box, files: files) { sent in self.transfers.update(t, { $0.done = sent }) }
                    close(fd)
                }
                if let w = why { var c: [UInt8] = [0x24]; c.u64(0); _ = self.request(c); self.transfers.finish(t, ok: false, reason: w) }
                else { self.transfers.finish(t, ok: true) }
            }
            return
        }
        var body: [UInt8] = []
        if let png = pb.data(forType: .png) { body = [2] + png }
        else if let tiff = pb.data(forType: .tiff), let rep = NSBitmapImageRep(data: tiff), let png = rep.representation(using: .png, properties: [:]) { body = [2] + png }
        else if let s = pb.string(forType: .string) { body = [1] + Array(s.utf8) }
        guard !body.isEmpty else { return }
        pushQ.async {
            let t = self.transfers.add(Transfer(toPeer: true, what: body[0] == 1 ? "text" : "picture", total: UInt64(body.count - 1)))
            if let r = self.request([0x22] + body), r.first == 0x02 { self.transfers.finish(t, ok: true) }
            else { self.transfers.finish(t, ok: false, reason: "\(otherComputer()) did not answer") }
        }
    }
    func setSmooth(_ v: Bool) {
        locked { cfg.smooth = v; cfg.save() }
        awdl.sync(want: v && locked({ mode != .idle }), force: true)
    }
    func setPaused(_ p: Bool) {
        update { $0.paused = p }
        if p { stopDriving(sendLeave: true, pos: nil, why: "paused"); endDriven(why: "paused") }
    }
}

private func tapCallback(proxy: CGEventTapProxy, type: CGEventType, event: CGEvent, refcon: UnsafeMutableRawPointer?) -> Unmanaged<CGEvent>? {
    let core = Unmanaged<Core>.fromOpaque(refcon!).takeUnretainedValue()
    return core.onTap(type, event)
}
