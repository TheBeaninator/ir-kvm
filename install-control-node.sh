#!/bin/bash
# Install the IR blaster tooling on a control node. Run from a copy of ~/ir-blaster on the node:
#   sudo bash install-control-node.sh
# Offline-friendly: needs only python3, python3-usb (pyusb) and libusb-1.0, which Ubuntu ships.
set -e
cd "$(dirname "$0")"
python3 -c 'import usb.core' 2>/dev/null || apt-get install -y python3-usb
python3 -c 'import serial' 2>/dev/null || apt-get install -y python3-serial
usermod -aG plugdev,dialout "${SUDO_USER:-$USER}"
install -m 644 99-tiqiaa-ir.rules /etc/udev/rules.d/
udevadm control --reload && udevadm trigger -s usb
ln -sf "$(pwd)/hdmi-switch" /usr/local/bin/hdmi-switch
ln -sf "$(pwd)/kvm" /usr/local/bin/kvm
ln -sf "$(pwd)/irkbd.py" /usr/local/bin/irkbd
ln -sf "$(pwd)/screen.py" /usr/local/bin/screen
command -v ffmpeg >/dev/null || apt-get install -y ffmpeg v4l-utils
usermod -aG video "${SUDO_USER:-$USER}"
echo "installed; plug in the transmitter (dongle or IR-LED node) and run: hdmi-switch list && hdmi-switch 1"
echo "(log out and back in for the plugdev/dialout group membership to apply)"
