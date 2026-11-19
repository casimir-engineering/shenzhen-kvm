#!/usr/bin/env python3
"""Shenzhen KVM, built into a Linux image (Ubuntu 22.04 arm64 tested).

For an image builder whose root object `r` offers r.path(rel), r.put(rel, bytes_or_path, feature, mode)
and r.enable(unit, feature=...):

    spec = importlib.util.spec_from_file_location('shenzhen_kvm_step', CHECKOUT / 'linux/image_step.py')
    step = importlib.util.module_from_spec(spec); spec.loader.exec_module(step); step.install(r)

install(r) compiles shenzhen-kvm.c with the image's own gcc (libsodium and libX11 headers must be in
the image) into /usr/local/bin/shenzhen-kvm, adds the tray app (Python 3 + GTK 3 through python3-gi),
its icons and menu entries, the service and its default settings, and enables the service. Nothing
that belongs to one computer goes into the image: no link (key and linked computer), no computer id,
no settings from a running install. Each computer makes its id at first start and is linked by its
person (`shenzhen-kvm visible` / `shenzhen-kvm link`, or the tray app).

The tray app starts with desktop sessions that read /etc/xdg/autostart; a session that starts its
programs itself needs `/usr/local/bin/shenzhen-kvm-tray &` after its panel.
"""
import subprocess
from pathlib import Path

HERE = Path(__file__).resolve().parent
FEATURE = 'shenzhen-kvm'
BIN = 'usr/local/bin/shenzhen-kvm'
UNIT = 'shenzhen-kvm.service'
FILES = {
    'usr/local/bin/shenzhen-kvm-tray': ('shenzhen-kvm-tray', 0o755),
    'etc/systemd/system/' + UNIT: ('files/' + UNIT, 0o644),
    'etc/shenzhen-kvm/config.json': ('files/config.json', 0o644),
    'etc/xdg/autostart/shenzhen-kvm-tray.desktop': ('files/shenzhen-kvm-tray.desktop', 0o644),
    'usr/share/applications/shenzhen-kvm-tray.desktop': ('files/shenzhen-kvm-tray.desktop', 0o644),
}
FILES.update({f'usr/share/icons/hicolor/{s}x{s}/apps/shenzhen-kvm.png': (f'files/icons/shenzhen-kvm-{s}.png', 0o644)
              for s in (16, 24, 32, 48, 64, 128, 256)})
# Must never be in an image: they belong to one computer.
PRIVATE = ('var/lib/shenzhen-kvm/link', 'var/lib/shenzhen-kvm/id', 'var/lib/shenzhen-kvm/settings.json')


def _need(ok, msg):
    if not ok:
        raise SystemExit('shenzhen-kvm: ' + msg)


def install(r):
    """r: the image builder's root object for the mounted image."""
    for h in ('usr/include/sodium.h', 'usr/include/X11/Xlib.h', 'usr/include/linux/uinput.h', 'usr/bin/gcc'):
        _need(r.path(h).exists(), 'the image has no ' + h + ' (gcc, libsodium-dev and libx11-dev are needed to build the service)')
    for g in ('usr/lib/python3/dist-packages/gi', 'usr/lib/aarch64-linux-gnu/girepository-1.0/Gtk-3.0.typelib'):
        _need(r.path(g).exists(), 'the image has no ' + g + ' (python3-gi and gir1.2-gtk-3.0 are needed for the tray app)')
    work = r.path('run/shenzhen-kvm-build')
    (work / 'src').mkdir(parents=True, exist_ok=True)
    try:
        (work / 'shenzhen-kvm.c').write_bytes((HERE / 'shenzhen-kvm.c').read_bytes())
        for part in sorted((HERE / 'src').glob('*.c')):
            (work / 'src' / part.name).write_bytes(part.read_bytes())
        subprocess.run(['chroot', str(r.p), 'gcc', '-O2', '-Wall', '-Wno-misleading-indentation', '-Wno-unused-result', '-Wno-format-truncation',
                        '-o', '/run/shenzhen-kvm-build/shenzhen-kvm', '/run/shenzhen-kvm-build/shenzhen-kvm.c', '-lsodium', '-lX11'], check=True)
        r.put(BIN, (work / 'shenzhen-kvm').read_bytes(), FEATURE, 0o755)
    finally:
        for p in sorted(work.rglob('*'), reverse=True):
            p.rmdir() if p.is_dir() else p.unlink()
        work.rmdir()
    for rel, (src, mode) in FILES.items():
        r.put(rel, HERE / src, FEATURE, mode)
    # GTK finds a theme's icons through its cache: without a fresh one the tray shows a fallback glyph.
    if r.path('usr/bin/gtk-update-icon-cache').exists():
        subprocess.run(['chroot', str(r.p), 'gtk-update-icon-cache', '-q', '-f', '/usr/share/icons/hicolor'], check=True)
    for p in PRIVATE:
        _need(not r.path(p).exists(), 'the image must not contain ' + p)
    r.enable(UNIT, feature=FEATURE)


if __name__ == '__main__':
    print(__doc__)
