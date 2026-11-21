# Shenzhen KVM on Linux

The Linux half of Shenzhen KVM: a service that shares this computer's keyboard, mouse and clipboard
with the linked computer (usually a Mac), and a top-bar app with the same window and menu as the Mac's.
Both follow `../PROTOCOL.md`.

## What a person sees

- **One pointer.** The pointer is on one screen at a time, and both computers' keyboards and mice
  act there, from wherever the pointer is. Push it past any edge of the screen (with either mouse) and
  it carries on on the other computer, coming in at the opposite edge at the same place along it; the
  screen it left hides its pointer. It doesn't cross while a button is held (dragging a window to an
  edge stays here), near a corner, or when it just reaches an edge and stops there: it takes a
  deliberate push. Both Shift keys bring it back to the computer whose
  keyboard you're using. The Mac's ⌘ acts as Control (⌘C copies),
  or as Super (a setting); ⌘Tab switches windows here (Alt+Tab, held while ⌘ is held), ⌃Tab stays
  Control+Tab, and the Mac's volume and playback keys work here.
- **Clipboard.** Copy anything here (any copy: Ctrl+C, a menu, right-click) and paste it on the other
  computer with its usual shortcut, and the other way round. Ctrl+C in a terminal stays an interrupt
  and sends nothing. Files from the other computer wait in `~/Downloads/From <its name>/<date time>/`
  and the clipboard then holds them (paste them in a file manager or an upload dialog). A paste pressed
  before they have all arrived waits for them. Once they are pasted (and the pasting program has
  finished reading them) they are removed; the 5 most recent copies never pasted are kept. A file one
  side already has from an earlier copy is not sent again. When the usual shortcuts are switched off,
  **Ctrl+Shift+C** copies here and sends, **Ctrl+Shift+V** pastes what the other computer copied.
- **Top-bar app** (`shenzhen-kvm-tray`): the app's own icon in the top bar (the same as on the Mac):
  in full colour while sharing works, dimmed when the computers aren't linked or sharing is off, grey
  when the service isn't running, with a small green dot while one computer's keyboard and mouse are
  in use on the other. Its menu says where the mouse
  is and lists transfers; **Open Shenzhen KVM…** opens the window: sharing on/off, linking (make this
  computer visible, the computers that are visible, the code to compare, forgetting the link), the
  clipboard settings and the transfers with their progress.
  The settings are shared with the other computer's window.
- While the pointer is on the other computer, this computer's pointer is hidden and its keyboard and
  mice act there; it shows again when the pointer comes back.
- If the other computer goes silent for 2 seconds while its keyboard is in use here, every key and
  button it was holding is released.
- **Which desktop.** The service uses the desktop on the lowest-numbered X display (the main one,
  `:1` on a computer whose desktop runs there). A computer that runs a second desktop on another
  display for a second screen (`:2`) keeps sharing with the main one only; `"display": ":2"` in
  `/etc/shenzhen-kvm/config.json` chooses another. The top-bar app runs once, in the main desktop's
  session (start it from that session only).

## Install on a running computer

As root, from a copy of this repository: `linux/install.sh`. It builds with this computer's gcc
(libsodium-dev, libx11-dev), installs

| what | where |
|------|-------|
| the service | `/usr/local/bin/shenzhen-kvm`, `shenzhen-kvm.service` (enabled, started) |
| the top-bar app | `/usr/local/bin/shenzhen-kvm-tray`, started with the desktop through `/etc/xdg/autostart/shenzhen-kvm-tray.desktop`, also in the applications menu |
| icons | `/usr/share/icons/hicolor/<size>/apps/shenzhen-kvm.png` |
| this computer's settings | `/etc/shenzhen-kvm/config.json`: `speed` (multiplier for the other computer's pointer here), `display` (which desktop) |

and keeps backups with an undo script (`/var/lib/shenzhen-kvm/undo.sh`, or `install.sh --undo`).
An earlier install of this service under another name (a systemd unit that runs `…kvm-share run`) is
stopped, disabled and moved into the backup, so the two never hold the keyboard at once; the undo
script puts it back. Its old settings and pairing key are not used: link the computers again.

A desktop session that does not read `/etc/xdg/autostart` needs `/usr/local/bin/shenzhen-kvm-tray &`
in its start script, after its panel. The top-bar app needs `python3-gi` and GTK 3 (`gir1.2-gtk-3.0`)
and a panel with a system tray (Xfce's "systray" plugin, for example).

## Command line

```
shenzhen-kvm visible        make this computer visible for 5 minutes and link with the computer that asks
shenzhen-kvm link [NAME]    list the visible computers and link with one (or the first whose name contains NAME)
shenzhen-kvm forget         forget the linked computer (it forgets this one too)
shenzhen-kvm status         the service's state as JSON
shenzhen-kvm bring-back     bring the pointer back to this computer
shenzhen-kvm settings '{"sharing": false}'   change shared settings (keys as in "settings" of status)
```

They work for root and for the person who owns the desktop (the user running the X server).

## How it works

- One C program, `shenzhen-kvm.c` (its parts are in `src/`), with libsodium (ChaCha20-Poly1305,
  X25519, SHA-256) and libX11; libXfixes is loaded at run time to watch the clipboard. Three
  processes: **main** keeps the input path (UDP packets to `/dev/uinput`, physical devices to UDP; no
  sleeps, no batching) and the linked computer's presence; **helper** owns the X connection (pointer
  queries, the clipboard) and the TCP clipboard channel; **agent** serves the control socket
  (`/run/shenzhen-kvm/control.sock`, newline-delimited JSON, used by the top-bar app and the command
  line), finds computers and links them.
- Input is injected through two uinput devices, `Shenzhen KVM keyboard` and `Shenzhen KVM pointer`,
  created once when the service starts and destroyed only when it stops: a settings or `config.json`
  change is applied in place, and a link change restarts the service with the devices handed over, so
  the desktop never sees them go and come back.
  The pointer device is absolute (0..65535): the service keeps the pointer position itself and applies
  the other computer's movements 1:1 times `speed`. Keyboards plugged in here are held by the service
  (EVIOCGRAB) and replayed through its keyboard, so shortcuts can be caught and go to the other
  computer while it drives it; mice are held only while driving the other computer.
- The X server and its auth file are found from the X server's command line (`Xwayland`, `Xorg`, `X`
  or `Xvfb`); the desktop's user is the owner of that process. A desktop restart is followed.
  Everything the service asks of the desktop (where the pointer is, the screen size, hiding the
  pointer, the clipboard, whether a terminal has focus) goes through the `desk_*` functions in
  `src/15-desk.c`; `src/16-desk-x11.c` implements them for X11, and a Wayland backend would implement
  the same functions.
- Files: `/var/lib/shenzhen-kvm/id` (this computer's id), `link` (the linked computer and its key,
  mode 0600), `settings.json` (the shared settings, from this computer's side). None of them belongs in
  a disk image: each computer makes its own id, and is linked by its person.
- Ports: UDP 24800 (input and presence), TCP 24801 (clipboard), UDP and TCP 24802 (finding and linking),
  mDNS on 5353 while visible or looking (it shares the port with Avahi or systemd-resolved).
- Tests: `SHENZHEN_KVM_CONF`, `SHENZHEN_KVM_STATE`, `SHENZHEN_KVM_RUN` move the folders,
  `SHENZHEN_KVM_PORT_BASE` the ports, `SHENZHEN_KVM_INTERFACES` and `SHENZHEN_KVM_METHODS` limit how
  computers are found (PROTOCOL.md).

## Disk images

`image_step.py` installs the same files into a mounted image root (see its docstring); it checks that
no link, id or settings are in the image.
