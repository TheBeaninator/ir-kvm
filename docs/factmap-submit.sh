#!/usr/bin/env bash
# Submit the ir-kvm knowledge to factmap. Run wherever the factmap archive is reachable:
#   FACTMAP_ACTOR=claude-laptop FACTMAP=/path/to/factmap bash docs/factmap-submit.sh
# FACTMAP defaults to whatever `factmap` is on PATH (the fleet shim). Articles land in the
# inbox as `submitted`; a curator approves them into search.
set -euo pipefail
F=${FACTMAP:-factmap}
export FACTMAP_ACTOR=${FACTMAP_ACTOR:-claude-laptop}
sub() { "$F" submit "$@" && echo "  submitted: $1"; }

sub "ir-kvm: IR-driven out-of-band KVM for the AI Pod (commands, node convention, repo)" \
  --kind reference --provenance empirical --outcome success \
  -t ir-kvm -t kvm -t pod -t hardware -t control-node \
  --alias "IR KVM" --alias "hdmi-switch" --alias "irkbd" --alias "screen grab" --alias "capture card" --alias "out-of-band console" \
  -s "How the pod control node sees and types on fleet machines with no agent on them: hdmi-switch N / irkbd --node N / screen.py grab; HDMI input N == XIAO node N; repo github.com/thebeaninator/ir-kvm" \
  -b "$(cat <<'MD'
# ir-kvm (github.com/thebeaninator/ir-kvm, Apache-2.0; local checkout ~/ir-kvm on the laptop)

A ~$30 out-of-band KVM built from infrared. One IR transmitter on the control machine drives a
5-to-1 HDMI switch (output -> USB capture stick) and any number of XIAO "IR keyboard" nodes, one
plugged into each target's USB. Nothing runs on the targets: works in BIOS, bootloader, kernel panic,
hung game. Proven 2026-09-28: typed at pod-control's login prompt over IR, read it back off the
capture; rebooted into AMI setup and enabled Wake On LAN by navigating the BIOS with screen grabs
between keypresses.

## Commands (control machine; install with `sudo bash install-control-node.sh`)
- `hdmi-switch N | next | list` — select HDMI input (codes in codes.json; this switch = NEC addr 0x80)
- `irkbd --node N type "text\n"` / `key ctrl-alt-t` / `release` — type on node N (US layout, ~70 chars/s)
- `screen grab -o out.jpg [--mode 1080p10|720p30]` — one frame; `screen serve` keeps the latest frame
  in /dev/shm/irkvm-screen.jpg so repeated grabs are instant; `screen stop`
- `kvm N [type ...|key ...]` — hdmi-switch N then irkbd on node N. Set IRKVM_SELF=<own input> so it
  re-triggers hot-plug on this machine's own connector after selecting itself (see the HDMI gotcha article).

## Convention
HDMI input N carries the machine whose XIAO has node id N. Node 0 = broadcast. Node id 255 = a
transmitter-only node (never types on its host). Node id is set over the XIAO's serial port: `id N`.

## Hardware
Tiqiaa "Tview" USB IR blaster (10c4:8468) OR a XIAO RP2040 node with an IR LED on D9 as transmitter
(auto-detected by irtx.py; IRKVM_TX overrides). Receivers: XIAO RP2040 (preferred: no radio) or XIAO
ESP32S3 + CH0038K 38 kHz IR receiver (OUT->D10, GND, 3V3). Capture: MacroSilicon MS2109 stick.
Related gotchas: HDMI switch hot-plug, MS2109 modes, Tiqiaa dongle limits (separate articles).
MD
)"

sub "Cheap HDMI switches drop hot-plug on unselected inputs; amdgpu never re-detects the display" \
  --kind gotcha --provenance empirical --outcome success \
  -t ir-kvm -t hdmi -t amdgpu -t strix-halo -t display \
  --alias "HDMI switch black screen" --alias "HPD" --alias "hot plug detect" --alias "EDID emulator" --alias "trigger_hotplug" --alias "disconnected after switching input" \
  -s "After switching an HDMI switch away from a Strix Halo box and back, its HDMI-A-1 stays 'disconnected' until a cable re-seat; fixes: EDID emulator dongle per input, video=HDMI-A-1:1920x1080@60e, or echo 1 > /sys/kernel/debug/dri/1/HDMI-A-1/trigger_hotplug" \
  -b "$(cat <<'MD'
Observed 2026-09-28 on pod-control (Bosgame BeyondMax, Ryzen AI MAX+ 395, amdgpu, Ubuntu 24.04 console)
behind a 5-to-1 IR HDMI switch: select another input and come back -> the box's
/sys/class/drm/card1-HDMI-A-1/status stays `disconnected`, monitor and capture stick go dark.
Physically re-seating the HDMI cable fixes it. The switch also hands the source an 800x600 EDID.

What did NOT work: `echo detect|on > .../status` (status flips to connected but enabled stays disabled),
chvt toggling, rebinding fbcon (vtcon1/bind), cycling DRM master. fbcon only sets a mode on a real
hot-plug event, and forcing the connector does not raise one.

What works:
1. Runtime, amdgpu only (no reboot): `echo 1 | sudo tee /sys/kernel/debug/dri/1/HDMI-A-1/trigger_hotplug`
   -> connected/enabled within ~2 s, mode 1920x1080. `kvm` does this automatically when IRKVM_SELF matches.
2. Durable for a box you control: kernel params `video=HDMI-A-1:1920x1080@60e drm.edid_firmware=HDMI-A-1:edid/1920x1080.bin`
   (`e` = keep enabled regardless of HPD; EDID override avoids the switch's 800x600).
3. Best for targets (needs nothing on them): a pass-through HDMI EDID emulator (~$8, 1080p60 type) on each
   switch input, so the source always sees a monitor.
When the AI switches away deliberately it is testing video and can re-kick; a dead box gets power-cycled,
which re-handshakes anyway. So this mostly matters for the control node's own screen.
MD
)"

sub "MacroSilicon MS2109 HDMI capture stick: only 1080p10 or 720p30 are clean on USB 2" \
  --kind gotcha --provenance empirical --outcome success \
  -t ir-kvm -t capture -t v4l2 -t ffmpeg -t hardware \
  --alias "534d:2109" --alias "USB Video" --alias "HDMI capture dongle" --alias "mjpeg decode errors" --alias "no signal frame" \
  -s "MS2109 (lsusb 534d:2109 'USB Video') is USB 2 inside: 1080p30/720p60 MJPEG overrun the bus and decode as garbage; use 1080p10 (text) or 720p30 (motion); first ~2 s of frames are blank; 'no signal' = flat gray value 7" \
  -b "$(cat <<'MD'
Facts measured 2026-09-28 with ffmpeg -f v4l2 -input_format mjpeg on the $10 "USB 3.0 HDMI Video Capture" stick:
- Modes: 1920x1080@10 clean, 1280x720@30 clean, 1920x1080@30 -> 50+ decode errors/black, 1280x720@60 -> black.
- The stick emits uniform blank frames for ~1.5-2 s after streaming starts: skip 2 x fps frames for a cold grab
  (screen.py does), or keep it streaming (`screen serve`, MJPEG passthrough, ~5 % CPU).
- With no HDMI signal it outputs a flat frame of value 7 in all channels (std 0). A blanked-but-connected
  console is different (real signal, black ~10). Use this to tell "target dark" from "no link".
- It exposes two /dev/video nodes (capture + metadata); pick the one that lists MJPG formats. Also a hidraw.
- Text on a 1080p console read via 1080p10 is crisp enough for vision models; the switch's 800x600 EDID
  softens it (see the HDMI switch hot-plug article).
- A DisplayLink "USB HDMI Adapter" (17e9:4301) is an OUTPUT device, not capture: no /dev/video.
MD
)"

sub "Tiqiaa Tview USB IR blaster (10c4:8468): driving it from Linux and its limits" \
  --kind reference --provenance empirical --outcome success \
  -t ir-kvm -t infrared -t usb -t pyusb -t hardware \
  --alias "Silicon Labs Tview" --alias "ZaZaRemote" --alias "IR blaster linux" --alias "tiqiaa_usb_ir" \
  -s "No kernel driver/lirc; pure pyusb (normanr/tiqiaa-usb-ir-py); dongle makes its own ~36.6 kHz carrier, host sends envelope bytes (16 us ticks, bit7=on); ~392-byte pulse buffer; acks ~before last pulses leave; its IR receiver is unreliable" \
  -b "$(cat <<'MD'
lsusb shows `10c4:8468 Silicon Labs Tview`, manufacturer string Tiqiaa (the ZaZaRemote phone-app blaster, ~$8).
Vendor-specific HID with bulk endpoints -> nothing binds; no /dev/lirc. Driver: tiqiaa_usb_ir.py
(Norman Rasmussen, from XenRE's reverse engineering; bundled in ir-kvm). udev: MODE 0660 GROUP plugdev.
- Transmit: host sends on/off envelope only, one byte per run: bit7 = emitter on, low 7 bits = duration in
  16 us ticks (max 127 = 2 ms per byte; longer runs span bytes). Carrier is fixed ~36.6 kHz; the frequency
  argument is ignored by the hardware. 38 kHz receivers are fine with it.
- Buffer limit: ~392 bytes of pulse data (7 x 56-byte USB fragments). Longer frames are silently truncated.
  A 20-byte IRKB frame (~323 bytes) fits; 24 bytes does not.
- Timing: the Data command blocks roughly the frame's air time, but the ack comes slightly BEFORE the last
  pulses are out; sending the next frame immediately corrupts it. Wait >= 0.1 x air time (+20 ms).
- After a replug/aborted transfer it can hold a stale reply -> first command's reply-id check fails;
  drain the reply queue after opening (irtx.py does).
- Receive mode works in principle (carrier-level pulses, not demodulated) but triggered once in many tries;
  don't design around it. Discover a device's codes by probing instead (irprobe.py Tk helper).
- Once dropped off the bus mid-command and re-enumerated by itself (crash); fine after retry.
MD
)"

sub "IRKB: addressed infrared keyboard bus (frame format, XIAO nodes, throughput)" \
  --kind pattern --provenance empirical --outcome success \
  -t ir-kvm -t protocol -t infrared -t xiao -t rp2040 -t esp32s3 -t usb-hid \
  --alias "IR keyboard" --alias "IR bus protocol" --alias "NEC timing" --alias "XIAO USB HID keyboard" --alias "irkbd firmware" \
  -s "One transmitter, up to 254 addressed receivers: 9 ms lead, lead space selects timing (4500 NEC / 2250 fast 350/350/1000 us), bytes dst|type<<4|seq|payload|CRC-8; string frames of 16 chars; ~70 chars/s, 0 CRC errors; XIAO RP2040/ESP32S3 firmware in ir-kvm" \
  -b "$(cat <<'MD'
Layer-2-style shared IR bus. Every receiver hears every frame and acts only on its own address.
Frame (LSB-first bytes after the lead): dst (0 = broadcast, 1..254), type<<4 | seq(4), payload, CRC-8 (poly 0x07, init 0).
Types: 0 chord (mods, HID keycode; press+release), 1 press/hold, 2 release-all,
3 string (n, then n x (HID key | shift<<7), n <= 16 because of the Tiqiaa 392-byte buffer).
Timing: 9000 us lead mark; lead space 4500 us = classic NEC bits (560 / 560 / 1690), 2250 us = fast bits
(350 mark, 350 = 0, 1000 = 1). Receiver picks thresholds per frame from the lead space. Sender sends once
(0 CRC errors observed over thousands of frames); `--twice` available; receiver drops a repeat of the last seq.
Measured: 4 chars/s (one NEC frame per key, sent twice) -> 28 (string frames) -> 44 (fast timing) -> 71 chars/s
(20 ms inter-frame gap). 245-char paragraph typed character-perfect.
Receiver firmware (irkbd-firmware.ino, Arduino): edge-capture ISR + state machine. Two bugs worth remembering:
(1) IR receiver OUT is active-low: a mark ENDS on the rising edge; (2) compare `micros() - lastEdge` as
signed - the ISR can update lastEdge between the two reads and the unsigned wrap reset the decoder mid-frame
(intermittent, worse without debug prints). USB HID: ESP32S3 USBHIDKeyboard (USBMode=default = TinyUSB OTG);
RP2040 raw reports via Adafruit_USBD_HID. RP2040 nodes can also transmit (IR LED on D9, 38 kHz PWM carrier,
serial `irkb <hex>` / `nec <addr> <cmd>` / `raw ...` / `ping`); id 255 = transmit-only. Prefer RP2040: no radio.
Wiring: receiver OUT->D10, GND->GND, VCC->3V3 (right column of the XIAO: 5V GND 3V3 D10 D9 ...).
MD
)"

sub "pod-control (Bosgame BeyondMax) BIOS: AMI setup entry, Auto Power On, Wake On LAN, MAC" \
  --kind config --provenance empirical --outcome success \
  -t pod -t control-node -t bios -t wake-on-lan -t bosgame -t strix-halo \
  --alias "pod-control BIOS" --alias "WoL pod-control" --alias "firmware-setup reboot" --alias "84:47:09:85:03:6c" \
  -s "pod-control: AMI Aptio BIOS 3.11 (2026-05-08); enter with `systemctl reboot --firmware-setup`; Auto Power On = Power On (AC restore); Wake On LAN enabled 2026-09-28 + NIC eno1 armed by udev rule (ethtool wol g); MAC 84:47:09:85:03:6c" \
  -b "$(cat <<'MD'
Read off the capture card 2026-09-28 (Main tab): BIOS/EC 3.11 built 2026-05-08, Bosgame Limited BeyondMax Series,
AMD Ryzen AI MAX+ 395 w/ Radeon 8060S @ 3.0 GHz, one M.2 NVMe Kingston OM8TAP42048K1-A00 2048 GB, second M.2 empty.
Advanced tab: Power Mode Select = Performance Mode; Auto Power On = Power On (the Matter power strips can
power-cycle it); Wake On LAN = Enabled (changed 2026-09-28 over the IR KVM; was Disabled); USB Boot Enabled;
SR-IOV Enabled. Submenus: S5 RTC Wake, GFX Configuration, Trusted Computing, CPU, Memory, Hardware Monitor, I/O Port Access.
Keys: F4 Save & Exit (dialog "Save configuration and exit?" Yes/No), F3 optimized defaults, ESC exit.
Enter setup from Linux: `sudo systemctl reboot --firmware-setup` (UEFI OsIndications). Boot messages appear on
HDMI ~20 s after reboot, setup screen right after.
OS side of WoL: BIOS alone leaves the NIC at `Wake-on: d`. Armed with `ethtool -s eno1 wol g` and persisted by
/etc/udev/rules.d/90-wol-eno1.rules (ACTION=="add", SUBSYSTEM=="net", KERNEL=="eno1", RUN+="/usr/sbin/ethtool -s eno1 wol g").
MAC eno1 = 84:47:09:85:03:6c. No wakeonlan client installed anywhere yet (a magic packet is 6x FF + 16x MAC to UDP 9 broadcast).
The 5:1 HDMI switch in front of it answers NEC address 0x80: in1 0x05, in2 0x09, in3 0x07, in4 0x1B, in5 0x08, next 0x1F.
MD
)"

sub "pkill -f with a pattern that appears in your own command line kills your own shell" \
  --kind gotcha --provenance empirical --outcome failure \
  -t shell -t agent -t gotcha \
  --alias "exit code 144" --alias "pkill self kill" --alias "command killed itself" \
  -s "In an agent tool call, `pkill -f 'foo bar'` matches the bash -c wrapper whose argv contains the same text, so the whole command dies (exit 144) before the rest runs; use a pidfile, `pkill -x`, or a [f]oo bracket trick that does not appear literally" \
  -b "$(cat <<'MD'
Bitten four times in one session (2026-09-28). The agent's Bash tool runs `bash -c '<entire command>'`, so any
`pkill -f PATTERN` whose PATTERN also matches text elsewhere in that same command line (e.g. a later
`ffmpeg ... irkvm-screen.jpg`) kills the wrapper shell itself. Symptom: the command returns exit code 144 with
no output and nothing after the pkill ran (edits not applied, tests not run).
Safe alternatives: have the daemon write a pidfile and kill that pid after checking /proc/<pid>/cmdline;
`pkill -x <exact-binary-name>` when unambiguous; or the `[f]oo` regex trick - `pkill -f '[i]rprobe\.py'` matches
"irprobe.py" but the literal "[i]rprobe" in your own argv does not match the regex - but only if no OTHER part
of the command line contains the plain text.
MD
)"
echo "all submitted"
