#!/bin/bash
# Install the IR blaster tooling on a control node. Run from a copy of ~/ir-blaster on the node:
#   sudo bash install-control-node.sh
# Offline-friendly: needs only python3, python3-usb (pyusb) and libusb-1.0, which Ubuntu ships.
set -e
cd "$(dirname "$0")"
python3 -c 'import usb.core' 2>/dev/null || apt-get install -y python3-usb
install -m 644 99-tiqiaa-ir.rules /etc/udev/rules.d/
udevadm control --reload && udevadm trigger -s usb
ln -sf "$(pwd)/hdmi-switch" /usr/local/bin/hdmi-switch
echo "installed; plug in the dongle and run: hdmi-switch list && hdmi-switch 1"
