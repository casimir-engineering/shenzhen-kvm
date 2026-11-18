#!/bin/bash
# Installs Shenzhen KVM on a running Linux computer (as root, from a copy of this folder):
#   install.sh          build with this computer's gcc, install, start the service
#   install.sh --undo   stop it and put back what was there before
# Needs gcc, libsodium-dev and libx11-dev to build; python3-gi and GTK 3 for the tray app.
# Linking is done afterwards: `shenzhen-kvm visible` or `shenzhen-kvm link`, or the tray app.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
state=/var/lib/shenzhen-kvm
bin=/usr/local/bin/shenzhen-kvm
tray=/usr/local/bin/shenzhen-kvm-tray
unit=/etc/systemd/system/shenzhen-kvm.service
conf=/etc/shenzhen-kvm/config.json
autostart=/etc/xdg/autostart/shenzhen-kvm-tray.desktop
appentry=/usr/share/applications/shenzhen-kvm-tray.desktop
[ "$(id -u)" = 0 ] || { echo 'run as root'; exit 1; }

if [ "${1:-}" = --undo ]; then
    [ -x "$state/undo.sh" ] || { echo "nothing to undo ($state/undo.sh missing)"; exit 1; }
    exec "$state/undo.sh"
fi

# 1. Build first (nothing changes if it fails).
tmp=$(mktemp -d)
trap 'rm -f "$tmp/shenzhen-kvm"; rmdir "$tmp"' EXIT
gcc -O2 -Wall -Wno-misleading-indentation -Wno-unused-result -Wno-format-truncation -o "$tmp/shenzhen-kvm" "$here/shenzhen-kvm.c" -lsodium -lX11

# 2. Backups and the undo script.
backup=$state/backup-$(date +%Y%m%dT%H%M%S)
mkdir -p "$backup"; chmod 700 "$state" "$backup"
files="$bin $tray $unit $conf $autostart $appentry"
for f in $files; do [ -e "$f" ] && cp -a "$f" "$backup/$(echo "$f" | tr / _)"; done
was_enabled=$(systemctl is-enabled shenzhen-kvm.service 2>/dev/null || true)

# 3. An earlier install of this service under another name (a unit that runs "<something>kvm-share run"):
#    stopped, disabled and moved into the backup, so the two never hold the keyboard at once.
earlier=""
for u in /etc/systemd/system/*.service; do
    [ "$u" = "$unit" ] && continue
    grep -qE '^ExecStart=/usr/local/bin/[a-z-]*kvm-share run$' "$u" 2>/dev/null || continue
    name=$(basename "$u"); exe=$(sed -n 's/^ExecStart=\([^ ]*\) run$/\1/p' "$u")
    systemctl stop "$name" 2>/dev/null || true
    systemctl disable "$name" 2>/dev/null || true
    cp -a "$u" "$backup/earlier-$name"; rm -f "$u"
    [ -n "$exe" ] && [ -e "$exe" ] && { cp -a "$exe" "$backup/earlier-$(basename "$exe")"; rm -f "$exe"; }
    earlier="$earlier $name:$exe"
    echo "Replaced the earlier service $name."
done

cat > "$backup/undo.sh" <<UNDO
#!/bin/bash
# Takes Shenzhen KVM out again, as before $(date +%Y-%m-%d). Stopping it releases every key and
# button it holds and gives the keyboard and mouse back. The link is kept in $state/link.
set -u
b=$backup
systemctl stop shenzhen-kvm.service 2>/dev/null
systemctl disable shenzhen-kvm.service 2>/dev/null
for f in $files; do
    k="\$b/\$(echo "\$f" | tr / _)"
    if [ -e "\$k" ]; then cp -a "\$k" "\$f"; else rm -f "\$f"; fi
done
for e in $earlier; do
    n=\${e%%:*}; x=\${e#*:}
    cp -a "\$b/earlier-\$n" "/etc/systemd/system/\$n"
    [ -n "\$x" ] && [ -e "\$b/earlier-\$(basename "\$x")" ] && cp -a "\$b/earlier-\$(basename "\$x")" "\$x"
    systemctl daemon-reload; systemctl enable --now "\$n"
done
systemctl daemon-reload
[ "$was_enabled" = enabled ] && systemctl enable --now shenzhen-kvm.service
echo 'Shenzhen KVM removed.'
UNDO
chmod 700 "$backup/undo.sh"
ln -sfn "$backup/undo.sh" "$state/undo.sh"

# 4. Install and (re)start once.
install -m755 "$tmp/shenzhen-kvm" "$bin"
install -m755 "$here/shenzhen-kvm-tray" "$tray"
install -m644 "$here/files/shenzhen-kvm.service" "$unit"
install -d -m755 /etc/shenzhen-kvm
[ -e "$conf" ] || install -m644 "$here/files/config.json" "$conf"
install -D -m644 "$here/files/shenzhen-kvm-tray.desktop" "$autostart"
install -D -m644 "$here/files/shenzhen-kvm-tray.desktop" "$appentry"
for s in 16 24 32 48 64 128 256; do
    install -D -m644 "$here/files/icons/shenzhen-kvm-$s.png" "/usr/share/icons/hicolor/${s}x${s}/apps/shenzhen-kvm.png"
done
gtk-update-icon-cache -q /usr/share/icons/hicolor 2>/dev/null || true
systemctl daemon-reload
systemctl enable shenzhen-kvm.service
systemctl restart shenzhen-kvm.service
echo "Shenzhen KVM is running. Link it: shenzhen-kvm visible (or shenzhen-kvm link), or open the tray app."
echo "Undo: $state/undo.sh  (or: install.sh --undo)"
