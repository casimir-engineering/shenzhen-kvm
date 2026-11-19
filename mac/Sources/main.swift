import AppKit
import ApplicationServices

updaterCommandLine(CommandLine.arguments)

// MARK: command line checks (no menu bar app): --hello | --ping N | --clip-get | --clip-put-text TEXT | --state

let args = CommandLine.arguments
let background = args.contains("--background")
if args.count > 1 && !background && args[1].hasPrefix("--") && !args[1].hasPrefix("-psn") {
    let core = Core()
    if args[1] == "--state" {
        let l = core.link
        print("this computer: \(computerName()) (\(myIDHex)); ports \(portBase)-\(discPort)")
        print(l.map { "linked with \($0.peerName) (\($0.peerIDHex))" } ?? "not linked")
        exit(0)
    }
    guard core.link != nil else { print("not linked"); exit(2) }
    // find the peer first: its presence answer gives the address
    core.finder.startPresence(udp4: core.udp.fd)
    Thread.detachNewThread { while true { if let (p, f) = core.udp.recv(), p.count > 7, Array(p[0..<6]) == MAGIC {
        var ss = sockaddr_storage(); withUnsafeMutablePointer(to: &ss) { $0.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee = f } }
        core.finder.handlePresent(p, ss) } } }
    let t0 = mono(); while core.locked({ core.peerIP }) == nil && mono() - t0 < 8 { core.finder.sendPresent(reply: true); Thread.sleep(forTimeInterval: 0.5) }
    guard let ip = core.locked({ core.peerIP }) else { print("the other computer was not found"); exit(1) }
    print("found at \(ip) by \(core.locked { core.foundBy })")
    let tcp: ([UInt8]) -> [UInt8]? = { core.request($0) }
    switch args[1] {
    case "--hello":
        let d = try! JSONSerialization.data(withJSONObject: ["name": "Mac check", "version": 1, "os": "mac"])
        guard let r = tcp([0x20] + Array(d)), r.first == 0x20 else { print("no answer"); exit(1) }
        print(String(decoding: r.dropFirst(), as: UTF8.self))
    case "--ping":
        let n = args.count > 2 ? Int(args[2]) ?? 50 : 50
        guard let u = UDP(port: 0), let a = core.peerAddr, let box = core.box else { exit(1) }
        var rtts: [Double] = []; var lost = 0
        for _ in 0..<n {
            let t0 = mono()
            u.send(box.seal([0x1F]), to: a)
            var got = false
            while mono() - t0 < 0.5, let (p, _) = u.recv(timeoutMs: 500) {
                if let pl = box.open(p, replayCheck: true), pl == [0x02, 0x1F] { got = true; break }
            }
            if got { rtts.append(mono() - t0) } else { lost += 1 }
            Thread.sleep(forTimeInterval: 0.02)
        }
        rtts.sort()
        if rtts.isEmpty { print("no answers"); exit(1) }
        print(String(format: "ALIVE round trip ms: median %.3f  p95 %.3f  worst %.3f  (n=%d lost=%d)",
                     rtts[rtts.count / 2] * 1000, rtts[min(rtts.count - 1, rtts.count * 95 / 100)] * 1000, rtts.last! * 1000, rtts.count, lost))
    case "--clip-get":
        guard let r = tcp([0x21]), r.first == 0x23, r.count > 1 else { print("no answer"); exit(1) }
        switch r[1] {
        case 1: print("text: " + String(decoding: r.dropFirst(2), as: UTF8.self))
        case 2: print("image: \(r.count - 2) bytes PNG")
        case 3:
            var rd = Reader(Array(r.dropFirst(2))); let c = rd.u16() ?? 0
            print("files: \(c)")
            for _ in 0..<c { guard let l = rd.u16(), let nb = rd.bytes(Int(l)), let s = rd.u64(), rd.bytes(Int(s)) != nil else { break }
                print("  \(String(decoding: nb, as: UTF8.self)) \(s) bytes") }
        default: print("empty")
        }
    case "--clip-put-text":
        guard args.count > 2, let r = tcp([0x22, 1] + Array(args[2].utf8)), r == [0x02, 0x22] else { print("failed"); exit(1) }
        print("ok")
    default:
        print("usage: --state | --hello | --ping N | --clip-get | --clip-put-text TEXT"); exit(2)
    }
    exit(0)
}

// MARK: app (one copy: a second one shows the first one's window and quits)

let me = NSRunningApplication.current
if let bid = Bundle.main.bundleIdentifier,
   NSRunningApplication.runningApplications(withBundleIdentifier: bid).contains(where: { $0.processIdentifier != me.processIdentifier }) {
    if !background { DistributedNotificationCenter.default().postNotificationName(Notification.Name(bid + ".show"), object: nil, userInfo: nil, deliverImmediately: true) }
    exit(0)
}
signal(SIGPIPE, SIG_IGN)
let app = NSApplication.shared
app.setActivationPolicy(.regular)
let delegate = AppDelegate()
delegate.showAtLaunch = !background
app.delegate = delegate
// On stop (launchctl, update, logout): give the pointer back before exiting.
signal(SIGTERM, SIG_IGN)
let termSource = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .global())
termSource.setEventHandler {
    if let core = delegate.core {
        core.stopDriving(sendLeave: false, pos: nil, why: "app stopped")   // the other computer keeps the pointer
        core.endDriven(why: "app stopped")
        core.awdl.restoreNow()
    }
    usleep(50_000)
    exit(0)
}
termSource.resume()
app.run()
