#!/bin/bash
# Removes the rule added by install-smooth.sh and turns AirDrop/Handoff's link (awdl0) back on.
set -u
sudo /sbin/ifconfig awdl0 up
sudo rm -f /etc/sudoers.d/shenzhen-kvm-awdl
echo "Removed. AirDrop and Handoff work as before."
