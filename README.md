# ir-kvm

A ~$30 out-of-band KVM built from infrared. One IR transmitter on a control machine (a XIAO RP2040
node with an IR LED, or a Tiqiaa USB blaster) drives:

- a **5-to-1 HDMI switch** (its normal IR remote codes) whose output feeds a USB capture card, and
- any number of **IR-bus USB keyboards**: a Seeed XIAO ESP32S3 with a 38 kHz IR receiver, plugged
  into each target machine, that enumerates as a plain USB HID keyboard and types whatever is
  addressed to it over the air.

Nothing runs on the targets. It works in the BIOS, in a bootloader, during a kernel panic, and in a
hung game, because to the target it is just a keyboard and a monitor. Built so an AI agent on a
control node can drive a rack of machines without depending on their CPUs at all.

```
control node ──USB──▶ Tiqiaa IR blaster ─ ─ IR ─ ─▶ HDMI 5:1 switch ──HDMI──▶ capture card ──USB──▶ control node
                                          └ ─ IR ─ ─▶ XIAO node 1 ──USB HID──▶ target 1
                                          └ ─ IR ─ ─▶ XIAO node 2 ──USB HID──▶ target 2 ...
```

Measured: ~70 characters/second typed, zero CRC errors over thousands of frames, 254 addressable
receivers on one blaster.

## Hardware

| Part | Notes |
|---|---|
| IR transmitter on the control machine | Either **a XIAO RP2040 or ATmega32U4 (DFRobot Beetle) node with an IR LED on D9** (same firmware, node id 255 = blaster only), or a Tiqiaa "Tview" USB IR transceiver (`lsusb`: `10c4:8468 Silicon Labs Tview`, ~$8, the ZaZaRemote phone-app blaster; vendor HID, no kernel driver, driven with pyusb). |
| IR LED + driver (for the transmitter node) | 940 nm IR LED, NPN transistor (2N2222/BC337/S8050), 1 kΩ base resistor, LED series resistor sized for ~100 mA pulses from 5V (~33 Ω). |
| 5-to-1 HDMI switch with IR remote | Ours answers NEC address `0x80`; codes in `codes.json`. Any IR-controlled switch works once you find its codes (see `irprobe.py`). |
| USB HDMI capture stick | Anything V4L2 sees. Tested: MacroSilicon MS2109 (`534d:2109 USB Video`), the $10 "USB 3.0 HDMI Video Capture" stick. It is USB 2 inside: 1080p10 or 720p30 are clean, 1080p30 / 720p60 overrun the bus. |
| Seeed XIAO RP2040 **or** XIAO ESP32S3 **or** an ATmega32U4 board (one per target) | XIAOs share a footprint and pinout. RP2040 is the recommended one: no WiFi/Bluetooth on the board at all. The 32U4 (DFRobot Beetle, Arduino Micro/Leonardo, Pro Micro) is the cheapest radio-free option and also transmits. |
| CH0038K / any 38 kHz 3-pin IR receiver (one per XIAO) | VS1838B, TSOP38238, CHQ1838 etc. all work. Active-low output. |

### Wiring the receiver to the XIAO / Beetle

USB-C at the top, looking at the top of the board, the right column is `5V, GND, 3V3, D10, ...`.
On the DFRobot Beetle the pads are labelled; it breaks out D9, D10, D11 and 3V3/5V/GND, so the
wiring below is identical.
Receiver (lens facing you, legs down) is usually `OUT, GND, VCC` left to right; check your part.

```
receiver OUT ─▶ D10 (GPIO9)
receiver GND ─▶ GND
receiver VCC ─▶ 3V3
```

Point the dome at the transmitter. Range is a few metres; the transmitter is not fussy about aim.

### Wiring the IR LED (transmitter node, RP2040 or 32U4)

```
D9 (GPIO4) ─ 1 kΩ ─ base of NPN
5V ─ 33 Ω ─ IR LED anode;  IR LED cathode ─ collector;  emitter ─ GND
```

The firmware puts a 38 kHz, 33 % duty carrier on D9 during marks. Driving the LED straight from the
pin (≈ 12 mA) works across a desk; the transistor gets you across the room. Set the node's id to 255
(`id 255` over serial) so it never types on the control machine, and leave its receiver unpopulated
or ignore it. The tools find it automatically (see `irtx.py`); `IRKVM_TX=/dev/ttyACM0` pins it.

## Install (control machine)

```
sudo bash install-control-node.sh     # udev rule for the dongle, python3-usb, symlinks
hdmi-switch list && hdmi-switch 1
```

Needs only `python3` plus `python3-serial` for a transmitter node, or `python3-usb` (pyusb) and
`libusb-1.0` for the Tiqiaa dongle. Works offline.

## Use

```
hdmi-switch 3                 # select HDMI input 3        (also: next, list, send 0xNN)
irkbd.py --node 3 type "ls -la\n"
irkbd.py --node 3 key ctrl-alt-t
irkbd.py --node 3 release
kvm 3 type "hello\n"          # switch to input 3 and type on node 3
```

```
screen.py grab -o now.jpg          # one frame from the capture stick (1080p10 by default, ~4 s cold)
screen.py serve &                  # keep the latest frame in /dev/shm/irkvm-screen.jpg; grab is then instant
screen.py stop | info
```

Convention: HDMI input *N* carries the machine whose XIAO has node id *N*. Node 0 is broadcast.
`irkbd.py` uses the US keyboard layout; `\n` = Enter, `\t` = Tab. Chords accept
`ctrl shift alt gui` (+ `r`-prefixed right-hand versions) and key names like `enter esc f5 up home`.

## Flash a receiver

One sketch, three boards. Uses [arduino-cli](https://arduino.github.io/arduino-cli/).

**XIAO RP2040** (core `rp2040:rp2040` from
`https://github.com/earlephilhower/arduino-pico/releases/download/global/package_rp2040_index.json`):

```
cd irkbd-firmware
arduino-cli compile --fqbn rp2040:rp2040:seeed_xiao_rp2040:usbstack=tinyusb --output-dir build-rp2040 .
arduino-cli upload  --fqbn rp2040:rp2040:seeed_xiao_rp2040:usbstack=tinyusb -p /dev/ttyACM0 --input-dir build-rp2040 .
```

A blank XIAO RP2040 shows up as a USB drive (RPI-RP2) instead of a serial port; then use
`arduino-cli upload ... -p <mount point>` or just copy `build-rp2040/irkbd-firmware.ino.uf2` onto it.

**XIAO ESP32S3** (core `esp32:esp32`):

```
cd irkbd-firmware
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default --output-dir build-esp32s3 .
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default -p /dev/ttyACM0 --input-dir build-esp32s3 .
```

The first ESP32S3 upload attempt often fails while the board re-enumerates into its bootloader;
just run it again.

**DFRobot Beetle / Arduino Micro / Leonardo / Pro Micro** (ATmega32U4, core `arduino:avr`, no
third-party index needed; the Beetle enumerates as `2341:8037 Arduino Micro`):

```
cd irkbd-firmware
arduino-cli core install arduino:avr
arduino-cli compile --fqbn arduino:avr:micro --output-dir build-avr32u4 .
arduino-cli upload  --fqbn arduino:avr:micro -p /dev/ttyACM0 --input-dir build-avr32u4 .
```

Uses ~1.3 KB of the 32U4's 2.5 KB RAM. The edge ring is 256 entries (fine: `loop()` drains it as
the frame arrives) and the `raw` serial command only holds 40 pulses, so on this board the host
tools use the on-board framing (`irkb`, `necb`, `nec`) which they do anyway. The receiver on D10
is serviced by a pin-change interrupt (the 32U4 has no INTx on D10), the carrier is Timer1 PWM on
D9 (OC1A), and the keyboard is a raw report-protocol HID keyboard through the core's HID library.
If a particular BIOS only enumerates boot-protocol keyboards, that one is the board to swap for an
RP2040.

Either way, then set the node id over the board's serial port (it keeps it in flash):

```
$ python3 -m serial.tools.miniterm /dev/ttyACM0 115200
id 3          -> node id = 3 (saved)        (255 = blaster only, never types)
ping          -> irkvm rp2040 id=3 tx=1     (or esp32s3 / avr32u4)
stat          -> pin=1 edges=... frames=... crcerr=...   (bring-up diagnostics)
raw on        -> dumps every pulse/space and decoder state transition
```

Transmitter nodes also take `irkb <hex>` (an IRKB frame, fast timing), `nec <addr> <cmd>` and
`raw +mark -space ...` and answer `ok` once the frame is out; that is what `irtx.py` speaks.

## Protocol ("IRKB")

One IR bus, layer-2 style: every receiver hears every frame and acts only on its own address.

```
lead: 9000 µs mark, then a lead space that selects the bit timing:
      4500 µs = NEC timing  (mark 560, space 560 = 0, 1690 = 1)
      2250 µs = fast timing (mark 350, space 350 = 0, 1000 = 1)   ← default
bytes, LSB first:
      dst        0 = broadcast, 1..254 = node id
      type<<4 | seq
      payload    type 0 chord / 1 press-and-hold / 2 release-all:  mods, HID keycode
                 type 3 string: n, then n × (HID keycode | shift<<7), n ≤ 16
      crc8       poly 0x07, init 0, over everything before it
```

Receivers drop a frame whose `seq` equals the last one they accepted, so a sender may repeat frames
for reliability (`--twice`). `--nec` falls back to the slower classic timing.

### Tiqiaa dongle quirks worth knowing

- It buffers about 392 bytes of pulse data (7 × 56-byte fragments); longer frames are truncated.
  Hence 16-character string frames.
- It acknowledges a transmit slightly *before* the last pulses leave. Sending the next frame
  immediately corrupts it; waiting 0.1 × the frame's air time is enough (`irkbd.py` does this).
- Its IR *receiver* was unreliable in testing (one capture in many tries), so the HDMI switch codes
  were found by probing with `irprobe.py` (a small Tk app: step through codes, label what works).
- The carrier frequency argument is ignored by the hardware (fixed ~36.6 kHz).

## HDMI switch gotcha: hot-plug detect

Cheap HDMI switches pull hot-plug low on every input that is not selected. When you switch away
from a machine and back, its GPU sees a disconnect and may never re-detect the display (seen with
amdgpu on the Strix Halo box: connector stays "disconnected" until the cable is physically
re-seated). Fixes, best first:

1. **EDID emulator pass-through dongles** on each switch input (the $5 "HDMI EDID emulator" /
   dummy-plug pass-throughs). The source then always sees a monitor. Needs nothing on the targets.
2. On a Linux box you control, force the output: kernel parameters
   `video=HDMI-A-1:1920x1080@60e drm.edid_firmware=HDMI-A-1:edid/1920x1080.bin` (the `e` keeps
   the output enabled regardless of hot-plug; the EDID override avoids inheriting the switch's
   own 800x600 EDID).
3. Runtime kick on amdgpu, no reboot:
   `echo 1 | sudo tee /sys/kernel/debug/dri/1/HDMI-A-1/trigger_hotplug`
   (forcing `status` to `on` alone is not enough: fbcon only sets a mode on a hot-plug event).

## Radios

The XIAO RP2040 has no radio, which is why it is the recommended receiver for an out-of-band device
you want to trust. The XIAO ESP32S3 build is kept because that is what the first units were; the
firmware never initialises its WiFi or Bluetooth, but the hardware is still there.

## Files

| File | What |
|---|---|
| `hdmi-switch` | CLI for the HDMI switch (`codes.json`) |
| `irkbd.py` | IR keyboard bus sender |
| `kvm` | `hdmi-switch` + `irkbd.py` in one call |
| `screen.py` | the V: grab a frame from the capture stick, or serve the latest one from /dev/shm |
| `irkbd-firmware/` | receiver firmware for XIAO RP2040 / ESP32S3 / ATmega32U4 (Arduino) |
| `irsend.py` | send one NEC frame (addr, cmd) |
| `irprobe.py` | Tk helper for discovering a device's IR codes |
| `irtx.py` | transmitter abstraction: Tiqiaa dongle or serial node, auto-detected |
| `tiqiaa_usb_ir.py` | Tiqiaa dongle driver (third-party, see NOTICE) |
| `99-tiqiaa-ir.rules` | udev rule (group `plugdev`) |
| `install-control-node.sh` | installer for the control machine |

## License

Apache-2.0 (see `LICENSE`). `tiqiaa_usb_ir.py` is by Norman Rasmussen, building on Xen (XenRE)'s
reverse engineering; see `NOTICE`.
