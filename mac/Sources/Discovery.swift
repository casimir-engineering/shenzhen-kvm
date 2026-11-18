import Foundation
import dnssd
import SystemConfiguration

// Finding computers (PROTOCOL.md Finding computers): visible / looking for linking, and the linked
// peer's presence so its address follows DHCP. Several methods at once, merged by computer id.

let MAGIC: [UInt8] = Array("SZKVM1".utf8)
let mcastGroup = "239.255.83.90"
let mdnsType = "_shenzhen-kvm._udp"

struct Methods: OptionSet {
    let rawValue: Int
    static let mdns = Methods(rawValue: 1), broadcast = Methods(rawValue: 2), multicast = Methods(rawValue: 4), ipv6 = Methods(rawValue: 8), sweep = Methods(rawValue: 16)
    static let all: Methods = [.mdns, .broadcast, .multicast, .ipv6, .sweep]
    static let names: [(Methods, String)] = [(.mdns, "mdns"), (.broadcast, "broadcast"), (.multicast, "multicast"), (.ipv6, "ipv6"), (.sweep, "sweep")]
    var name: String { Methods.names.first { $0.0 == self }?.1 ?? "direct" }
    init(rawValue: Int) { self.rawValue = rawValue }
    init(name: String) { self = Methods.names.first { $0.1 == name }?.0 ?? .sweep }
}
/// SHENZHEN_KVM_METHODS=mdns,broadcast,... switches methods off for tests.
let enabledMethods: Methods = {
    guard let e = ProcessInfo.processInfo.environment["SHENZHEN_KVM_METHODS"], !e.isEmpty else { return .all }
    var m: Methods = []
    for (b, n) in Methods.names where e.contains(n) { m.insert(b) }
    return m
}()

struct Iface: Equatable {
    var name: String, index: UInt32
    var addr = in_addr(), mask = in_addr(), bcast = in_addr()
    var v4 = false, v6 = false
    static func == (a: Iface, b: Iface) -> Bool { a.name == b.name && a.index == b.index && a.addr.s_addr == b.addr.s_addr && a.mask.s_addr == b.mask.s_addr && a.v4 == b.v4 && a.v6 == b.v6 }
}
private let onlyIfaces: [String]? = ProcessInfo.processInfo.environment["SHENZHEN_KVM_INTERFACES"].flatMap { $0.isEmpty ? nil : $0.split(separator: ",").map(String.init) }

/// Interfaces used to find computers: up, not loopback, not point-to-point, not tunnels or VPNs, not AirDrop's.
func scanInterfaces() -> [Iface] {
    var list: UnsafeMutablePointer<ifaddrs>?
    guard getifaddrs(&list) == 0 else { return [] }
    defer { freeifaddrs(list) }
    var out: [Iface] = []
    var p = list
    while let a = p {
        p = a.pointee.ifa_next
        guard let sa = a.pointee.ifa_addr else { continue }
        let name = String(cString: a.pointee.ifa_name), flags = Int32(a.pointee.ifa_flags)
        if let only = onlyIfaces { if !only.contains(name) { continue } }
        else {
            if flags & IFF_UP == 0 || flags & IFF_LOOPBACK != 0 || flags & IFF_POINTOPOINT != 0 { continue }
            if ["utun", "tun", "tap", "ppp", "ipsec", "wg", "awdl", "llw", "anpi", "gif", "stf", "ap"].contains(where: { name.hasPrefix($0) }) { continue }
        }
        var i = out.firstIndex { $0.name == name } ?? -1
        if i < 0 { out.append(Iface(name: name, index: if_nametoindex(name))); i = out.count - 1 }
        if Int32(sa.pointee.sa_family) == AF_INET && !out[i].v4 {
            out[i].v4 = true
            out[i].addr = sa.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr }
            if let m = a.pointee.ifa_netmask { out[i].mask = m.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { $0.pointee.sin_addr } }
            else { out[i].mask.s_addr = UInt32(0xffffff00).bigEndian }
            out[i].bcast.s_addr = (out[i].addr.s_addr & out[i].mask.s_addr) | ~out[i].mask.s_addr
        } else if Int32(sa.pointee.sa_family) == AF_INET6 {
            let ll = sa.withMemoryRebound(to: sockaddr_in6.self, capacity: 1) { $0.pointee.sin6_addr.__u6_addr.__u6_addr8.0 == 0xfe && ($0.pointee.sin6_addr.__u6_addr.__u6_addr8.1 & 0xc0) == 0x80 }
            if ll { out[i].v6 = true }
        }
    }
    return out.filter { $0.v4 || $0.v6 }
}
func ipString(_ a: in_addr) -> String { var a = a; var b = [CChar](repeating: 0, count: 16); inet_ntop(AF_INET, &a, &b, 16); return String(cString: b) }
func ipAddr(_ s: String) -> in_addr? { var a = in_addr(); return inet_pton(AF_INET, s, &a) == 1 ? a : nil }

/// IPv4 and IPv6 UDP sockets on one port, plus an unbound socket for 255.255.255.255 out of one interface.
final class DSock {
    let s4: Int32, s6: Int32, sb: Int32
    let port: UInt16
    init(port: UInt16, existing4: Int32? = nil) {
        self.port = port
        var one: Int32 = 1
        let f4: Int32
        if let e = existing4 { f4 = e } else {
            f4 = socket(AF_INET, SOCK_DGRAM, 0)
            setsockopt(f4, SOL_SOCKET, SO_REUSEADDR, &one, 4); setsockopt(f4, SOL_SOCKET, SO_REUSEPORT, &one, 4)
            var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET); a.sin_port = port.bigEndian
            if withUnsafePointer(to: &a, { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(f4, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } }) != 0 {
                log("cannot use UDP port \(port): \(String(cString: strerror(errno)))")
            }
        }
        setsockopt(f4, SOL_SOCKET, SO_BROADCAST, &one, 4)
        let f6 = socket(AF_INET6, SOCK_DGRAM, 0)
        setsockopt(f6, IPPROTO_IPV6, IPV6_V6ONLY, &one, 4)
        setsockopt(f6, SOL_SOCKET, SO_REUSEADDR, &one, 4); setsockopt(f6, SOL_SOCKET, SO_REUSEPORT, &one, 4)
        var a6 = sockaddr_in6(); a6.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size); a6.sin6_family = sa_family_t(AF_INET6); a6.sin6_port = port.bigEndian; a6.sin6_addr = in6addr_any
        _ = withUnsafePointer(to: &a6) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { bind(f6, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } }
        let fb = socket(AF_INET, SOCK_DGRAM, 0)
        setsockopt(fb, SOL_SOCKET, SO_BROADCAST, &one, 4)
        s4 = f4; s6 = f6; sb = fb
    }
    func close(ownV4: Bool) { if ownV4 { Darwin.close(s4) }; Darwin.close(s6); Darwin.close(sb) }

    func join(_ ifs: [Iface]) {
        for i in ifs {
            if i.v4 {
                var m = ip_mreq(); m.imr_interface = i.addr; inet_pton(AF_INET, mcastGroup, &m.imr_multiaddr)
                setsockopt(s4, IPPROTO_IP, IP_ADD_MEMBERSHIP, &m, socklen_t(MemoryLayout<ip_mreq>.size))
            }
            if i.v6 {
                var m = ipv6_mreq(); m.ipv6mr_interface = i.index; inet_pton(AF_INET6, "ff02::1", &m.ipv6mr_multiaddr)
                setsockopt(s6, IPPROTO_IPV6, IPV6_JOIN_GROUP, &m, socklen_t(MemoryLayout<ipv6_mreq>.size))
            }
        }
    }
    static func send4(_ s: Int32, _ b: [UInt8], _ ip: in_addr, _ port: UInt16) {
        var a = sockaddr_in(); a.sin_len = UInt8(MemoryLayout<sockaddr_in>.size); a.sin_family = sa_family_t(AF_INET); a.sin_port = port.bigEndian; a.sin_addr = ip
        _ = b.withUnsafeBytes { p in withUnsafePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, p.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in>.size)) } } }
    }
    static func sendTo(_ s: Int32, _ b: [UInt8], _ to: sockaddr_storage) {
        var t = to; let len = socklen_t(to.ss_len)
        _ = b.withUnsafeBytes { p in withUnsafePointer(to: &t) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s, p.baseAddress, b.count, 0, $0, len) } } }
    }
    /// One datagram per method and interface: make(method) builds it.
    func send(to dport: UInt16, methods want: Methods, ifs: [Iface], make: (Methods) -> [UInt8]) {
        for m in [Methods.broadcast, .multicast, .ipv6] where want.contains(m) && enabledMethods.contains(m) {
            let b = make(m)
            for i in ifs {
                if m == .broadcast && i.v4 {
                    DSock.send4(s4, b, i.bcast, dport)
                    var idx = i.index; setsockopt(sb, IPPROTO_IP, 25 /* IP_BOUND_IF */, &idx, 4)
                    DSock.send4(sb, b, in_addr(s_addr: INADDR_BROADCAST), dport)
                } else if m == .multicast && i.v4 {
                    var ifa = i.addr; setsockopt(s4, IPPROTO_IP, IP_MULTICAST_IF, &ifa, socklen_t(MemoryLayout<in_addr>.size))
                    DSock.send4(s4, b, ipAddr(mcastGroup)!, dport)
                } else if m == .ipv6 && i.v6 {
                    var idx = i.index; setsockopt(s6, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, 4)
                    var to = sockaddr_in6(); to.sin6_len = UInt8(MemoryLayout<sockaddr_in6>.size); to.sin6_family = sa_family_t(AF_INET6); to.sin6_port = dport.bigEndian
                    to.sin6_scope_id = i.index; inet_pton(AF_INET6, "ff02::1", &to.sin6_addr)
                    _ = b.withUnsafeBytes { p in withUnsafePointer(to: &to) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { sendto(s6, p.baseAddress, b.count, 0, $0, socklen_t(MemoryLayout<sockaddr_in6>.size)) } } }
                }
            }
        }
    }
    static func recv(_ s: Int32) -> ([UInt8], sockaddr_storage)? {
        var buf = [UInt8](repeating: 0, count: 9000)
        var a = sockaddr_storage(); var l = socklen_t(MemoryLayout<sockaddr_storage>.size)
        let n = buf.withUnsafeMutableBytes { p in withUnsafeMutablePointer(to: &a) { $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { recvfrom(s, p.baseAddress, 9000, MSG_DONTWAIT, $0, &l) } } }
        if n < 0 { return nil }
        return (Array(buf[0..<n]), a)
    }
}

/// Unicast sweep of each interface's subnet (at most the 1024 addresses around this Mac), a few per call.
struct Sweep {
    var active = false
    private var iface = -1, next: UInt32 = 0, end: UInt32 = 0
    mutating func start() { guard enabledMethods.contains(.sweep) else { return }; active = true; iface = -1; next = 0; end = 0 }
    /// Returns addresses to probe now (up to budget) with the interface index into ifs.
    mutating func step(_ ifs: [Iface], budget: Int) -> [in_addr] {
        var out: [in_addr] = []
        while active && out.count < budget {
            if next >= end {
                repeat { iface += 1 } while iface < ifs.count && !ifs[iface].v4
                if iface >= ifs.count { active = false; break }
                let a = UInt32(bigEndian: ifs[iface].addr.s_addr), mk = UInt32(bigEndian: ifs[iface].mask.s_addr)
                let net = a & mk, last = net | ~mk
                var lo = net + 1, hi = last &- 1
                if hi > lo && hi - lo > 1023 { lo = a > lo + 512 ? a - 512 : lo; if lo + 1023 < hi { hi = lo + 1023 } }
                next = lo; end = hi &+ 1
                continue
            }
            let h = next; next += 1
            if h.bigEndian == ifs[iface].addr.s_addr { continue }
            out.append(in_addr(s_addr: h.bigEndian))
        }
        return out
    }
}

/// A computer found while looking.
struct Found {
    var id: String, name: String, ip: String, dp: UInt16, by: Methods, seen: Double
}

/// Picks a peer's IPv4: the packet's own IPv4 source, else one of its "addrs" on a subnet of ours, else its first.
func pickAddress(_ j: [String: Any], _ src: sockaddr_storage?, _ ifs: [Iface]) -> String? {
    if let s = src, Int32(s.ss_family) == AF_INET {
        var t = s; return withUnsafePointer(to: &t) { $0.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { ipString($0.pointee.sin_addr) } }
    }
    let addrs = (j["addrs"] as? [String] ?? []).compactMap { ipAddr($0) }
    for a in addrs { for i in ifs where i.v4 && (a.s_addr & i.mask.s_addr) == (i.addr.s_addr & i.mask.s_addr) { return ipString(a) } }
    return addrs.first.map(ipString)
}

func myAddrs(_ ifs: [Iface]) -> [String] { ifs.filter { $0.v4 }.map { ipString($0.addr) } }

func computerName() -> String {
    (SCDynamicStoreCopyComputerName(nil, nil) as String?) ?? Host.current().localizedName ?? "Mac"
}

// MARK: Bonjour (the system's mDNS): register while visible, browse while looking

private var bonjourFinder: Finder?
private func bonjourIfIndex() -> UInt32 { onlyIfaces?.first.map { if_nametoindex($0) } ?? 0 }

final class Bonjour {
    private var reg: DNSServiceRef?, browse: DNSServiceRef?
    private var resolves: [String: DNSServiceRef] = [:]
    let q = DispatchQueue(label: "bonjour")

    func register(name: String, dp: UInt16) {
        guard enabledMethods.contains(.mdns), reg == nil else { return }
        var txt: [UInt8] = []
        for kv in ["id=\(myIDHex)", "name=\(name)", "dp=\(dp)", "v=1"] { let b = Array(kv.utf8.prefix(255)); txt.append(UInt8(b.count)); txt += b }
        var ref: DNSServiceRef?
        let err = DNSServiceRegister(&ref, 0, bonjourIfIndex(), String(name.prefix(63)), mdnsType, nil, nil, discPort.bigEndian, UInt16(txt.count), txt, { _, _, err, _, _, _, _ in
            if err != kDNSServiceErr_NoError { log("Bonjour: could not announce (\(err))") }
        }, nil)
        guard err == kDNSServiceErr_NoError, let r = ref else { log("Bonjour: could not announce (\(err))"); return }
        DNSServiceSetDispatchQueue(r, q); reg = r
    }
    func unregister() { if let r = reg { DNSServiceRefDeallocate(r) }; reg = nil }

    func startBrowsing(_ f: Finder) {
        guard enabledMethods.contains(.mdns), browse == nil else { return }
        bonjourFinder = f
        var ref: DNSServiceRef?
        let err = DNSServiceBrowse(&ref, 0, bonjourIfIndex(), mdnsType, nil, { _, flags, ifIndex, err, name, type, domain, _ in
            guard err == kDNSServiceErr_NoError, flags & kDNSServiceFlagsAdd != 0, let name, let type, let domain else { return }
            bonjourFinder?.bonjour.resolve(String(cString: name), String(cString: type), String(cString: domain), ifIndex)
        }, nil)
        guard err == kDNSServiceErr_NoError, let r = ref else { log("Bonjour: could not look (\(err))"); return }
        DNSServiceSetDispatchQueue(r, q); browse = r
    }
    func stopBrowsing() {
        if let r = browse { DNSServiceRefDeallocate(r) }; browse = nil
        for (_, r) in resolves { DNSServiceRefDeallocate(r) }; resolves = [:]
    }

    fileprivate func resolve(_ name: String, _ type: String, _ domain: String, _ ifIndex: UInt32) {
        let k = "\(name)|\(ifIndex)"
        if let old = resolves[k] { DNSServiceRefDeallocate(old) }
        var ref: DNSServiceRef?
        let err = DNSServiceResolve(&ref, 0, ifIndex, name, type, domain, { sdRef, _, ifIndex, err, _, host, _, txtLen, txt, _ in
            guard err == kDNSServiceErr_NoError, let host, let txt else { return }
            var kv: [String: String] = [:]
            var o = 0; let p = UnsafeBufferPointer(start: txt, count: Int(txtLen))
            while o < p.count { let l = Int(p[o]); o += 1; if o + l > p.count { break }
                let s = String(decoding: p[o..<o + l], as: UTF8.self); o += l
                if let eq = s.firstIndex(of: "=") { kv[String(s[..<eq])] = String(s[s.index(after: eq)...]) } }
            let hostName = String(cString: host)
            bonjourFinder?.bonjour.lookUp(hostName, ifIndex, kv)
        }, nil)
        if err == kDNSServiceErr_NoError, let r = ref { DNSServiceSetDispatchQueue(r, q); resolves[k] = r }
    }
    private var lookups: [DNSServiceRef] = []
    private var pending: [String: [String: String]] = [:]
    fileprivate func lookUp(_ host: String, _ ifIndex: UInt32, _ kv: [String: String]) {
        pending[host] = kv
        var ref: DNSServiceRef?
        let err = DNSServiceGetAddrInfo(&ref, 0, ifIndex, DNSServiceProtocol(kDNSServiceProtocol_IPv4), host, { sdRef, _, _, err, host, addr, _, _ in
            guard err == kDNSServiceErr_NoError, let host, let addr, Int32(addr.pointee.sa_family) == AF_INET else { return }
            let ip = addr.withMemoryRebound(to: sockaddr_in.self, capacity: 1) { ipString($0.pointee.sin_addr) }
            if let f = bonjourFinder, let kv = f.bonjour.pending[String(cString: host)] { f.foundBy(.mdns, id: kv["id"] ?? "", name: kv["name"] ?? "", ip: ip, dp: UInt16(kv["dp"] ?? "") ?? 24800) }
        }, nil)
        if err == kDNSServiceErr_NoError, let r = ref { DNSServiceSetDispatchQueue(r, q); lookups.append(r); if lookups.count > 32 { DNSServiceRefDeallocate(lookups.removeFirst()) } }
    }
}

// MARK: Finder: visible / looking (linking port), and the linked peer's presence (input port)

final class Finder {
    private var L = os_unfair_lock()
    func locked<T>(_ f: () -> T) -> T { os_unfair_lock_lock(&L); defer { os_unfair_lock_unlock(&L) }; return f() }
    let bonjour = Bonjour()
    private(set) var ifs: [Iface] = scanInterfaces()
    private var disc: DSock?
    private var sweep = Sweep()
    private(set) var visibleUntil = 0.0            // mono s; 0 = not visible
    private(set) var looking = false
    private var found: [String: Found] = [:]
    var onChange: (() -> Void)?
    var onRequestSocket: ((Bool) -> Void)?         // visible on/off: the linker listens for requests
    let name = computerName()
    private var nextAnnounce = 0.0, nextLook = 0.0, nextSweep = 0.0, nextScan = 0.0

    var visibleLeft: Int { let l = visibleUntil - mono(); return visibleUntil > 0 && l > 0 ? Int(l.rounded(.up)) : 0 }
    var foundList: [Found] { locked { found.values.filter { mono() - $0.seen < 45 }.sorted { $0.name < $1.name } } }

    init() {
        let t = Thread { [weak self] in self?.loop() }
        t.qualityOfService = .utility; t.name = "finder"; t.start()
    }

    func setVisible(_ on: Bool) {
        locked { visibleUntil = on ? mono() + 300 : 0; nextAnnounce = 0; if on { nextSweep = mono() + 30; sweep.start() } }
        syncSockets()
        if on { bonjour.q.async { self.bonjour.register(name: self.name, dp: portBase) }; log("visible for 5 minutes") }
        else { bonjour.q.async { self.bonjour.unregister() } }
        onRequestSocket?(on)
        onChange?()
    }
    func setLooking(_ on: Bool) {
        let was = locked { () -> Bool in let w = looking; looking = on; if on && !w { found = [:]; nextLook = 0; nextSweep = mono() + 0.5 }; return w }
        if was == on { return }
        syncSockets()
        if on { bonjour.q.async { self.bonjour.startBrowsing(self) } } else { bonjour.q.async { self.bonjour.stopBrowsing() } }
        onChange?()
    }
    private func syncSockets() {
        let want = locked { visibleUntil > 0 || looking }
        locked {
            if want && disc == nil { ifs = scanInterfaces(); disc = DSock(port: discPort); disc!.join(ifs) }
            if !want, let d = disc { d.close(ownV4: true); disc = nil; sweep.active = false }
        }
    }

    func foundBy(_ m: Methods, id: String, name: String, ip: String, dp: UInt16) {
        guard !id.isEmpty, id != myIDHex, enabledMethods.contains(m), locked({ looking }) else { return }
        let new = locked { () -> Bool in
            var f = found[id] ?? Found(id: id, name: name, ip: ip, dp: dp, by: [], seen: 0)
            let n = !f.by.contains(m) || f.ip != ip
            f.name = name.isEmpty ? "Computer" : name; f.ip = ip; f.dp = dp; f.by.insert(m); f.seen = mono()
            found[id] = f; return n
        }
        if new { log("found \(name) at \(ip) by \(m.name)"); DispatchQueue.main.async { self.onChange?() } }
    }

    private func makeDisc(_ type: UInt8, _ m: Methods) -> [UInt8] {
        var j: [String: Any] = ["id": myIDHex, "name": name, "dp": Int(portBase), "addrs": myAddrs(ifs), "via": m.name, "v": 1]
        if type == 0x02 { j["left"] = visibleLeft }
        return MAGIC + [type] + Array(try! JSONSerialization.data(withJSONObject: j))
    }
    private func handle(_ pkt: [UInt8], _ src: sockaddr_storage, _ s: Int32) {
        guard pkt.count > 8, Array(pkt[0..<6]) == MAGIC, pkt[6] == 0x01 || pkt[6] == 0x02,
              let j = try? JSONSerialization.jsonObject(with: Data(pkt[7...])) as? [String: Any],
              let id = j["id"] as? String, id != myIDHex else { return }
        let m = Methods(name: j["via"] as? String ?? "sweep")
        guard enabledMethods.contains(m) else { return }
        if pkt[6] == 0x01 && locked({ visibleUntil > mono() }) { DSock.sendTo(s, makeDisc(0x02, m), src) }
        else if pkt[6] == 0x02, let ip = pickAddress(j, src, ifs) {
            foundBy(m, id: id, name: j["name"] as? String ?? "", ip: ip, dp: UInt16(j["dp"] as? Int ?? 24800))
        }
    }

    // Presence of the linked peer (PROTOCOL.md PRESENT), on the input port: Core feeds packets in.
    var pres: DSock?
    weak var core: Core?
    private var presentHigh: UInt64 = 0
    private var nextPresent = 0.0, presNextSweep = 0.0, presSweep = Sweep()
    func startPresence(udp4: Int32) { locked { if pres == nil { pres = DSock(port: portBase, existing4: udp4); pres!.join(ifs); nextPresent = 0 } } }
    func makePresent(_ m: Methods, reply: Bool) -> [UInt8]? {
        guard let core, let box = core.box, let link = core.link else { return nil }
        let j: [String: Any] = ["id": myIDHex, "dp": Int(portBase), "addrs": myAddrs(ifs), "reply": reply, "via": m.name]
        return MAGIC + [0x03] + link.linkID + box.seal(Array(try! JSONSerialization.data(withJSONObject: j)))
    }
    func sendPresent(reply: Bool) {
        guard let core, let link = core.link, let p = locked({ pres }) else { return }
        let dport = core.locked { core.peerDP } ?? link.peerDP
        p.send(to: dport, methods: [.broadcast, .multicast, .ipv6], ifs: ifs) { makePresent($0, reply: reply) ?? [] }
        if let ip = core.locked({ core.peerIP }).flatMap(ipAddr), let b = makePresent([], reply: reply) { DSock.send4(p.s4, b, ip, dport) }
    }
    /// A datagram starting with MAGIC on the input port (v4 from Core's socket, v6 from ours).
    func handlePresent(_ pkt: [UInt8], _ src: sockaddr_storage) {
        guard let core, let link = core.link, let box = core.box, pkt.count >= 15 + 28, pkt[6] == 0x03, Array(pkt[7..<15]) == link.linkID,
              let plain = box.open(Array(pkt[15...]), replayCheck: false) else { return }
        var c: UInt64 = 0; for k in 19..<27 { c = c << 8 | UInt64(pkt[k]) }
        guard locked({ () -> Bool in if c <= presentHigh { return false }; presentHigh = c; return true }),
              let j = try? JSONSerialization.jsonObject(with: Data(plain)) as? [String: Any], j["id"] as? String == link.peerIDHex else { return }
        let missing = !core.peerFound
        if let dp = j["dp"] as? Int, dp > 0, dp < 65536 { core.locked { core.peerDP = UInt16(dp) } }
        if let ip = pickAddress(j, src, ifs) { core.heard(ip: ip, by: j["via"] as? String ?? "direct") }
        if j["reply"] as? Bool == true || missing { sendPresent(reply: false) }
    }

    private func loop() {
        while true {
            let (d, p) = locked { (disc, pres) }
            var fds: [pollfd] = []
            if let d { fds += [pollfd(fd: d.s4, events: Int16(POLLIN), revents: 0), pollfd(fd: d.s6, events: Int16(POLLIN), revents: 0), pollfd(fd: d.sb, events: Int16(POLLIN), revents: 0)] }
            if let p { fds += [pollfd(fd: p.s6, events: Int16(POLLIN), revents: 0), pollfd(fd: p.sb, events: Int16(POLLIN), revents: 0)] }
            let busy = locked { sweep.active || presSweep.active }
            if fds.isEmpty { Thread.sleep(forTimeInterval: 0.25) } else { _ = poll(&fds, nfds_t(fds.count), busy ? 50 : 250) }
            if let d {
                for s in [d.s4, d.s6, d.sb] { while let (pkt, src) = DSock.recv(s) { handle(pkt, src, s) } }
            }
            if let p { for s in [p.s6, p.sb] { while let (pkt, src) = DSock.recv(s) { if pkt.count > 7, Array(pkt[0..<6]) == MAGIC { handlePresent(pkt, src) } } } }
            tick()
        }
    }
    private func tick() {
        let now = mono()
        let (vis, look, d) = locked { (visibleUntil > now, looking, disc) }
        if visibleUntil > 0 && !vis { setVisible(false) }
        if now >= nextScan {
            nextScan = now + 10
            let n = scanInterfaces()
            if n != ifs { locked { ifs = n }; d?.join(n); locked { pres }?.join(n); nextPresent = now; log("network changed") }
        }
        if let d, vis, now >= nextAnnounce { nextAnnounce = now + 2; d.send(to: discPort, methods: .all, ifs: ifs) { makeDisc(0x02, $0) } }
        if let d, look, now >= nextLook { nextLook = now + 2; d.send(to: discPort, methods: .all, ifs: ifs) { makeDisc(0x01, $0) } }
        if let d, vis || look {
            if now >= nextSweep && !sweep.active { nextSweep = now + 30; locked { sweep.start() } }
            if sweep.active { let b = makeDisc(look ? 0x01 : 0x02, .sweep); for a in locked({ sweep.step(ifs, budget: 10) }) { DSock.send4(d.s4, b, a, discPort) } }
        }
        if vis || look { DispatchQueue.main.async { self.onChange?() } }   // countdown and expiry
        // presence of the linked peer
        if let core, core.link != nil, let p = locked({ pres }) {
            let online = core.peerFound
            if now >= nextPresent {
                let missingFor = now - core.locked { core.lastHeard }
                let sweepNow = !online && missingFor > 30 && now >= presNextSweep
                if sweepNow { presNextSweep = now + 120; presSweep.start() }
                sendPresent(reply: !online)
                nextPresent = now + (online ? 30 : 5)
            }
            if presSweep.active, let b = makePresent(.sweep, reply: true) {
                let dport = core.locked { core.peerDP } ?? portBase
                for a in presSweep.step(ifs, budget: 10) { DSock.send4(p.s4, b, a, dport) }
            }
        }
    }
    func presenceNow() { nextPresent = 0 }
}
