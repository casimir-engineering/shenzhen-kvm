#!/bin/bash
# Smooth pointer over Wi-Fi: lets Shenzhen KVM pause AirDrop and Handoff's radio link (awdl0) while the
# mouse is on your other computer, so this Mac's Wi-Fi stays on the network's channel. Installs one
# sudoers rule (needs your password once), replaces an earlier rule for the same two commands, then
# measures the difference. Undo: uninstall-smooth.sh (next to this file in the repository).
set -euo pipefail
USERNAME=$(id -un)
RULE=/etc/sudoers.d/shenzhen-kvm-awdl
echo "Shenzhen KVM keeps the pointer smooth over Wi-Fi by pausing AirDrop and Handoff while the mouse is on your other computer."
echo "This lets it run only '/sbin/ifconfig awdl0 down' and '/sbin/ifconfig awdl0 up', without your password ($RULE)."
echo "macOS asks for your password once now."
echo

tmp=$(mktemp)
trap 'rm -f "$tmp"' EXIT
cat > "$tmp" <<RULE
# Shenzhen KVM: pause AirDrop/Handoff (awdl0) while the mouse is on the other computer.
$USERNAME ALL=(root) NOPASSWD: /sbin/ifconfig awdl0 down, /sbin/ifconfig awdl0 up
RULE
sudo visudo -cf "$tmp" >/dev/null
sudo install -m 0440 -o root -g wheel "$tmp" "$RULE"
# An earlier rule for the same two commands (from an earlier build) is no longer needed.
for f in /etc/sudoers.d/*; do
  [ "$f" = "$RULE" ] && continue
  if sudo grep -qs "^$USERNAME ALL=(root) NOPASSWD: /sbin/ifconfig awdl0 down, /sbin/ifconfig awdl0 up$" "$f"; then
    sudo rm -f -- "$f"; echo "Removed the earlier rule $f."
  fi
done
sudo -n -l /sbin/ifconfig awdl0 down >/dev/null && echo "Done. Shenzhen KVM uses it within 10 seconds."

# Proof: 15 s of ping to the router with awdl0 as it is, then 15 s with it paused.
GW=$(netstat -rn -f inet | awk '$1=="default" && $NF=="en0"{print $2; exit}')
stats() { sed -n 's/.*time=\([0-9.]*\).*/\1/p' | sort -n | awk '{v[++n]=$1; if ($1>20) s++} END{
  printf "median %.1f ms, worst %.1f ms, over 20 ms: %d of %d\n", v[int(n/2)+1], v[n], s, n}'; }
if [ -n "$GW" ]; then
  echo "Measuring Wi-Fi to the router ($GW), 15 s each (keep the mouse on this Mac meanwhile)..."
  sudo -n /sbin/ifconfig awdl0 up
  printf "  AirDrop/Handoff on:     "; ping -i 0.1 -c 150 "$GW" | stats
  sudo -n /sbin/ifconfig awdl0 down
  printf "  AirDrop/Handoff paused: "; ping -i 0.1 -c 150 "$GW" | stats
  sudo -n /sbin/ifconfig awdl0 up
fi
