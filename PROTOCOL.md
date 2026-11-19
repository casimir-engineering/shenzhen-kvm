# Shenzhen KVM: wire protocol

Two computers on the same network share one pointer, keyboard and clipboard. They are symmetric:
the pointer is on one screen at a time (that computer "holds" it), and the keyboards and mice of
both computers act there. Folders: `mac/` (the Mac app), `linux/` (the Linux service and its
`shenzhen-kvm` command). Both follow this file exactly; change it here first.

## Ports

| port | transport | use |
|------|-----------|-----|
| 24800 | UDP, IPv4 and IPv6 | input events (sealed); presence of the linked computer (PRESENT) |
| 24801 | TCP | clipboard and control (sealed) |
| 24802 | UDP, IPv4 and IPv6 | finding computers to link with: probes and answers |
| 24802 | TCP | linking two computers |
| 5353 | UDP multicast | mDNS, service type `_shenzhen-kvm._udp` |

The three numbers move together: a test install may use another base (`SHENZHEN_KVM_PORT_BASE`, e.g.
34800 gives 34800/34801/34802). Every discovery message carries its sender's ports, so a peer always
answers on the ports the other side really uses.

## Identity and link

- **Computer id**: 16 random bytes made at first start, kept for good (Mac: `id` in the app's folder;
  Linux: `/var/lib/shenzhen-kvm/id`). Shown nowhere; never in an image. Written as 32 lowercase hex
  digits in messages. Its first 4 bytes are the computer's **sender id** in nonces (below).
- **Name**: the computer's own name (Mac: the computer name in Sharing settings; Linux: the host
  name). People see the other computer by this name.
- **Link**: made by linking (below) and kept until forgotten. It holds the peer's id and name, the
  shared 32-byte key, and the 8-byte link id. One link per computer: making a new one replaces the
  old one. Mac: `link.json` in the app's folder, mode 0600. Linux: `/var/lib/shenzhen-kvm/link`, mode
  0600, root only. Neither is ever in an image or in git.

## Encryption (every sealed packet)

ChaCha20-Poly1305 with the link key. Packet = `nonce (12 bytes) || ciphertext || tag (16 bytes)`.
Nonce = sender id (4 bytes) + 8 bytes big-endian counter. The counter starts at the sender's start
time in microseconds since 1970 and rises with every packet (`max(previous + 1, now in µs)`, shared
by all processes of one computer), so it is never reused across restarts. A receiver accepts only
packets whose sender id is the linked peer's. It drops a UDP packet whose counter is not higher than
the highest it has accepted from that sender (no replay); TCP frames must arrive in order.

## Finding computers (UDP 24802 and mDNS)

A computer is **visible** for 5 minutes when its person asks for it (Mac: "Make this Mac visible";
Linux: `shenzhen-kvm visible`), and **looking** while its person is choosing a computer to link to
(Mac: while the window shows the linking section; Linux: `shenzhen-kvm link`). A linked computer
also keeps finding its peer (PRESENT), so addresses follow DHCP changes; no address is ever configured.
The peer is "found" while an authentic packet from it arrived in the last 40 s.

Several methods run at once and their results are merged by computer id. Each one can be switched
off for tests (`SHENZHEN_KVM_METHODS=mdns,broadcast,multicast,ipv6,sweep`, default all), and every
find is logged with the method that made it.

| method | how |
|--------|-----|
| `broadcast` | to each interface's subnet broadcast address and to 255.255.255.255 sent out of each interface |
| `multicast` | to the IPv4 group 239.255.83.90, sent out of each interface |
| `ipv6` | to ff02::1 (all nodes on the link) on each interface |
| `sweep` | unicast to every address of each interface's IPv4 subnet (at most the 1024 around this computer), at most 200 packets a second, every 30 s while looking or visible, every 2 minutes while a linked peer is missing |
| `mdns` | while visible, a service of type `_shenzhen-kvm._udp` (instance "<name>", TXT `id`, `name`, `dp` data port, `v=1`) on port 24802; while looking, a browse for it. Mac: the system's Bonjour. Linux: a small responder of its own on 5353 (it shares the port with Avahi when Avahi runs) |

Interfaces used: up, not loopback, not point-to-point and not tunnels (`utun*`, `tun*`, `ppp*`,
`ipsec*`, `wg*`), not Apple's peer-to-peer links (`awdl*`, `llw*`). A VPN is therefore never used to
find a computer; the physical interfaces are probed one by one. `SHENZHEN_KVM_INTERFACES=en0,bridge100`
limits this to the named interfaces (tests).

Datagrams: `"SZKVM1"` (6 bytes), u8 type, then the body.

| type | name | body | sent |
|------|------|------|------|
| 0x01 | LOOKING | UTF-8 JSON `{"id", "name", "dp", "addrs", "via", "v": 1}` | to port 24802 by a looking computer, every 2 s by each method (the sweep every 30 s); visible computers answer each one with VISIBLE, unicast to the packet's source address and port, with the same `via` |
| 0x02 | VISIBLE | UTF-8 JSON `{"id", "name", "dp", "addrs", "left": seconds of visibility left, "via", "v": 1}` | answer to LOOKING; also every 2 s by each method while visible (the sweep every 30 s) |
| 0x03 | PRESENT | u8[8] link id, then a sealed packet (above) whose plaintext is JSON `{"id", "dp", "addrs", "reply": true/false, "via", "holds": true/false}` (`holds`: the sender holds the pointer and got it from the other computer or took it back, see One pointer) | to the linked computer's input port (24800) by a linked computer: every 5 s while its peer is missing (and a sweep every 2 minutes once it has been missing for 30 s), every 30 s once found, at start and when the network changes; also unicast to its last address. The peer answers a PRESENT whose `reply` is true, or any PRESENT while it considers the sender missing, with its own (`reply` false), unicast to the source |

`dp` is the sender's input port (UDP); its clipboard port is `dp + 1` and its linking port `dp + 2`.
`addrs` lists the sender's IPv4 addresses on the interfaces above. `via` names the method that carried
the message (`mdns`, `broadcast`, `multicast`, `ipv6`, `sweep`, or `direct` for unicast to a known
address); a receiver ignores messages whose method it has switched off, and logs the method of every
find. A computer found while looking stays listed 45 s after it was last heard. The receiver uses the IPv4 source
address of the packet; for a packet that came over IPv6 or mDNS it uses the address in `addrs` that
shares a subnet with one of its own interfaces (else the first). An authentic PRESENT, and any
authentic sealed packet on 24800/24801, updates the peer's address at once. A computer never
answers its own messages (same id).

## Linking (TCP 24802)

The looking computer L connects to the visible computer V's linking port. Frames are a u32
big-endian length followed by UTF-8 JSON; byte strings are lowercase hex. V handles one request at
a time and refuses others while one is open; the connection closes after 3 minutes without a result.

1. L → V `{"t": "request", "id", "name", "dp", "pub": X25519 public key of a fresh key pair,
   "commit": SHA-256("shenzhen-kvm commit v1" || pub_L || n_L)}` with `n_L` 32 random bytes.
2. V → L `{"t": "hello", "id", "name", "dp", "pub": pub_V, "nonce": n_V}` (32 random bytes).
3. L → V `{"t": "reveal", "nonce": n_L}`. V checks the commitment and closes on a mismatch.
4. Both compute
   - `th = SHA-256("shenzhen-kvm link v1" || pub_L || pub_V || n_L || n_V || id_L || id_V)` (ids as 16 bytes),
   - `code = (first 8 bytes of SHA-256("shenzhen-kvm code v1" || th), big-endian) mod 1 000 000`,
     shown as two groups of three digits ("042 917"),
   - `key = HKDF-SHA256(ikm = X25519(own private, other public), salt = th, info = "shenzhen-kvm key v1", 32 bytes)`
     (an all-zero X25519 result ends the attempt),
   - `link id = first 8 bytes of SHA-256("shenzhen-kvm link id v1" || th)`.
5. Each side shows the code and its person confirms that the other screen shows the same one
   (V: "<L> wants to link. Code … Link / Don't Link"; L: "Does <V> show the code …?"). Then it sends
   `{"t": "match", "proof": HMAC-SHA256(key, "shenzhen-kvm match v1" || role)}` with role the byte
   `L` or `V`, or `{"t": "cancel"}` and closes. A side keeps the link once it has sent its own match
   and received a valid match from the other; the connection then closes.

Because L commits to `n_L` before it sees `n_V`, and V's `n_V` is fixed before it sees `n_L`, a
computer in the middle cannot choose keys that give both people the same code: it succeeds with
probability 1 in 10^6 per attempt, and every attempt is a request someone sees and declines.

## One pointer: who holds it

There is one pointer for both computers. At any time it is on exactly one screen; that computer is
the **holder**, the other is the **remote**.

- The holder shows its pointer. Its own keyboard and mouse act on it as usual, and it applies every
  event the remote sends (below) from wherever its pointer is at that moment, so nothing jumps when
  both mice are used one after the other.
- The remote hides its pointer and keeps it parked (Mac: hidden and detached from the mouse; Linux:
  its mice held with EVIOCGRAB, its pointer hidden with XFixes). It swallows all its local keyboard
  and mouse input, including the shortcuts its system keeps for itself (the Mac reads keys at the HID
  level), and sends it to the holder. So any mouse or keyboard, on either computer, acts on the screen
  that has the pointer.
- Only the holder moves the pointer across the edge, whichever mouse pushed it there: when its
  pointer is pushed past the edge that leads to the other computer (no button held), it hands the
  pointer over with ENTER and, once it has the ACK, becomes the remote. The holder therefore
  arbitrates when both mice move at once: their movements add up on its screen, and one crossing
  happens or none.
- Each computer starts as the holder of its own pointer. When the other computer is not answering,
  a computer is the holder.
- **Race.** Both computers can only believe they hold the pointer at the same time before their first
  handover (or after a restart). If both send ENTER at once, the computer with the lower id (bytewise)
  keeps its handover: it does not acknowledge the other's ENTER, and the other acknowledges and takes
  the pointer. Any ENTER received while not handing over is acknowledged and makes the receiver the
  holder (a repeated ENTER within 1.5 s of the one that made it the holder is acknowledged again but
  does not move the pointer).

## Input channel: UDP port 24800 on both peers

Plaintext = one or more events back to back. All integers big-endian.

| type | name   | body | sent by |
|------|--------|------|---------|
| 0x01 | ENTER  | u8 edge (0 left, 1 right, 2 top, 3 bottom: the edge of the RECEIVER's screen space the pointer comes in through), u16 pos (0..65535 along that edge, left→right or top→bottom) | the holder, handing the pointer over; repeated every 200 ms until ACK, for at most 1 s (then it stays the holder and does not try again for 1 s). The receiver becomes the holder with its pointer just inside that edge at `pos`; the sender becomes the remote on the ACK |
| 0x02 | ACK    | u8 of the type acknowledged | either |
| 0x03 | LEAVE  | u16 pos (ignored) | the remote, taking the pointer back to its own screen (both Shift keys, or "Bring the mouse back now"): it becomes the holder at once with its pointer where it was parked, and repeats LEAVE every 200 ms until ACK, for at most 1.5 s. The receiver releases everything it injected and becomes the remote |
| 0x10 | MOVE   | i16 dx, i16 dy (remote's pointer units; the holder applies them 1:1 times its `speed` setting, from its pointer's current position) | remote |
| 0x11 | BUTTON | u8 button (1 left, 2 right, 3 middle, 4 back, 5 forward), u8 down (1/0) | remote |
| 0x12 | SCROLL | i16 dx, i16 dy in 1/120 notch units (positive dy = the view scrolls down towards the end of the page, as a wheel turned towards the user: Linux REL_WHEEL -1 = dy +120; positive dx = the view scrolls right) | remote |
| 0x13 | KEY    | u16 USB HID keyboard usage (page 0x07, e.g. 0x04 = A, 0xE0..0xE7 modifiers), u8 down (1/0). One down per press: the remote never sends auto-repeat downs; the holder generates key repeat itself while a key is held | remote |
| 0x04 | STOPPING | none | the holder, when it stops with a claim (see Restarts); the remote answers ACK(0x04) and waits up to 30 s instead of 2 s before taking the pointer back |
| 0x1F | ALIVE  | none | the remote, every 500 ms; the holder answers each ALIVE with ACK(0x1F) |

Rules:
- Replies (ACK) go to the source address and port of the packet being answered.
- **Restarts keep the pointer where it is.** A computer "holds with a claim" from the moment it gets
  the pointer from the other computer (ENTER) or takes
  it back (both Shift keys, the menu, its watchdog), until it hands it over. Its PRESENT messages say
  `holds: true` then. A computer that starts (or restarts) without a claim and hears `holds: true`
  from its peer becomes the remote at once (its pointer hidden, its input forwarded).
  A computer that stops (quits, restarts, updates) while it is the remote sends nothing: the other
  keeps the pointer. One that stops while it holds the pointer with a claim sends STOPPING and keeps
  its claim for 60 s (Mac: in its preferences; Linux: `/var/lib/shenzhen-kvm/held`); the remote then
  waits up to 30 s for it instead of 2 s, and a restart within 60 s starts with the claim, so the
  pointer stays on its screen. Without STOPPING (a crash) the remote's 2 s watchdog applies.
- **Watchdogs.** The holder releases every key and button it injected after 2 s without any packet
  from the remote (it stays the holder). The remote takes the pointer back after 2 s without any
  packet from the holder: it becomes the holder with its pointer where it was parked.
- The holder starts with all injected keys up; whenever it stops applying the remote's events (handover,
  LEAVE, watchdog, sharing switched off) it releases every key and button it injected.
- When a computer switches sharing off, or the direction that lets the other computer's keyboard
  and mouse control it, it takes the pointer back (LEAVE) if it is the remote and stops applying the
  remote's events. A holder hands the pointer over only when its own settings let its keyboard and
  mouse control the other computer, and a computer accepts ENTER only when its settings let the other
  computer's keyboard and mouse control it.
- Media keys travel as the usages 0xE8 play/pause, 0xEA previous, 0xEB next, 0xED volume up, 0xEE
  volume down, 0xEF mute (the Mac keeps brightness and keyboard light). Modifier mapping is done by
  the HOLDER from `command_as` (`control`, the default: on Linux the Mac's Command acts as Control and
  its Control as Super; on the Mac, Linux Control acts as Command and Super as Control; `super`:
  Command and Super correspond, and Control stays Control). Option = Alt both ways. The app switcher
  keeps its meaning: on Linux, the Mac's Command+Tab becomes Alt+Tab with Alt held until Command is
  released (Shift+Tab goes back), and its Control+Tab stays Control+Tab; on the Mac, Linux Alt+Tab
  becomes ⌘Tab held the same way, and Linux Control+Tab stays ⌃Tab.
- Both peers send ENTER/LEAVE/BUTTON/KEY immediately, one datagram each; MOVE and SCROLL may be
  merged only with events already pending in the same event-loop turn (no timers, no batching delay).

## Clipboard and control channel: TCP port 24801 on both peers

One connection per request, TCP_NODELAY. Frame = u32 length + sealed packet (as above).
Plaintext = u8 type + body.

| type | name | body |
|------|------|------|
| 0x20 | HELLO | UTF-8 JSON `{"name": "...", "version": 1, "os": "mac" or "linux"}`; reply is the peer's HELLO. Sent by the Mac every 3 s once the peer's address is known; the name is the one people see |
| 0x21 | CLIP_GET | none. Reply: one CLIP_DATA |
| 0x22 | CLIP_PUT | same body as CLIP_DATA. Reply: ACK (0x02) |
| 0x23 | CLIP_DATA | u8 kind (0 empty, 1 text, 2 image, 3 files) then: text = UTF-8; image = PNG bytes; files = u16 count, then per file: u16 name length, UTF-8 relative path (folders are sent as their files, paths keep the folder name), u64 size, bytes |
| 0x24 | CLIP_PENDING | u64 total bytes. "Files were copied here and are on their way"; a FILES_BEGIN follows on another connection. 0 = the transfer was abandoned. Reply: ACK |
| 0x25 | CLIP_SKIPPED | u64 total bytes, u64 limit bytes. Files were copied here but are over the size limit and are not sent (so the other side can say so). Reply: ACK |
| 0x26 | SETTINGS | UTF-8 JSON from the sender's side, sent by either computer when a setting changes in its window (or command line) and when it finds its peer. Reply: ACK. Keys: `sharing` (all of it on/off), `i_drive_you` (the sender's keyboard and mouse may control the receiver), `you_drive_me`, `clipboard`, `usual_shortcuts` (true: shared clipboard below; false: the dedicated shortcuts), `big_files`, `file_limit_mb`, `my_edge` (the sender's edge that leads to the receiver: the receiver's is its opposite), `command_as` (`control` or `super`: what a Mac's Command key acts as on Linux; the Mac's Control is then the other one), `changed` (ms since 1970 of the change). The newest change wins: the receiver adopts the settings when `changed` is newer than its own, and otherwise answers with its own SETTINGS. Both keep them across restarts |
| 0x27 | FILES_BEGIN | Files in pieces (every file transfer). u16 count, then per file: u16 name length, UTF-8 relative path, u64 size. It is the first frame on its connection; FILES_DATA frames follow on the same connection, then the receiver writes the files, puts them on its clipboard and replies ACK (0x02 0x27); it closes the connection without an ACK if it fails |
| 0x28 | FILES_DATA | raw bytes, at most 1 MiB per frame: the files' contents back to back, in the order of FILES_BEGIN. Each frame is sealed on its own; both sides measure progress from these frames |
| 0x29 | FORGET | none. "The person forgot this link here": the receiver forgets it too. Reply: ACK |

Any authentic request on this channel also updates the peer's address (its source address).

Received files are written to a fresh folder (`~/Downloads/From <other computer's name>/<date time>/`,
on Linux in the home of the user who owns the desktop) and the local clipboard is set to those files
(file URLs on the Mac; `text/uri-list` and `x-special/gnome-copied-files` on Linux). Path components
`..` and absolute paths are refused.

## Shared clipboard (`usual_shortcuts` true, the default)

No key is intercepted. Each side watches its own clipboard (Mac: pasteboard change count; Linux:
XFixes selection-owner events on CLIPBOARD) and, while the pointer is on that computer, pushes every
change to the other side at once with CLIP_PUT. A clipboard set by the service itself (from a
received CLIP_PUT) is never pushed back. Text and images are pushed directly. Files: if the total
is over the limit and `big_files` is false, CLIP_SKIPPED is sent instead; otherwise CLIP_PENDING
first, then FILES_BEGIN + FILES_DATA in the background. While a CLIP_PENDING is open (until its
files arrive, it is cancelled, its FILES_BEGIN connection fails, or 10 minutes pass) the receiving
side holds back a paste shortcut (Command+V on the Mac; Control+V / Control+Shift+V on Linux) and
performs it when the files are in place. CLIP_GET of an empty clipboard never replaces a non-empty one.

## Dedicated shortcuts (`usual_shortcuts` false; handled by the computer whose keyboard is used)

"Here" = the computer the pointer is on right now; "there" = the other one.
- Command+Shift+C (Control+Shift+C on a Linux keyboard): copy the selection here (the normal copy
  shortcut is performed here), wait until the clipboard here changes (up to 300 ms), then CLIP_PUT
  it there. So the content is now on both clipboards.
- Command+Shift+V (Control+Shift+V): CLIP_GET from there, set the clipboard here, then perform the
  normal paste shortcut here.
The shortcut keys themselves are swallowed and never forwarded.

## Edges

Each peer names which edge of ITS whole screen space (all its screens together, outer boundary
only) leads to the other peer, e.g. Mac `edge = "top"`, Linux `edge = "bottom"`. Crossing happens
when the pointer is pushed against that outer boundary and no mouse button is held (so dragging a
window to the edge never crosses). Inner boundaries between a computer's own screens never cross.
