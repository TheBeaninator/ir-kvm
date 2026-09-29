# ir-kvm

A ~$30 out-of-band KVM built from infrared. One USB IR blaster on a control machine drives:

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
| Tiqiaa "Tview" USB IR transceiver | `lsusb`: `10c4:8468 Silicon Labs Tview`. Sold for the ZaZaRemote / Tiqiaa phone app, ~$8. Vendor HID with bulk endpoints; no kernel driver, driven with pyusb. Makes its own ~36.6 kHz carrier. |
| 5-to-1 HDMI switch with IR remote | Ours answers NEC address `0x80`; codes in `codes.json`. Any IR-controlled switch works once you find its codes (see `irprobe.py`). |
| USB HDMI capture card | Anything V4L2 sees. |
| Seeed XIAO ESP32S3 (one per target) | Native USB OTG → TinyUSB HID keyboard. See "Radios" below. |
| CH0038K / any 38 kHz 3-pin IR receiver (one per XIAO) | VS1838B, TSOP38238, CHQ1838 etc. all work. Active-low output. |

### Wiring the receiver to the XIAO

USB-C at the top, looking at the top of the board, the right column is `5V, GND, 3V3, D10, ...`.
Receiver (lens facing you, legs down) is usually `OUT, GND, VCC` left to right; check your part.

```
receiver OUT ─▶ D10 (GPIO9)
receiver GND ─▶ GND
receiver VCC ─▶ 3V3
```

Point the dome at the blaster. Range is a few metres; the blaster is not fussy about aim.

## Install (control machine)

```
sudo bash install-control-node.sh     # udev rule for the dongle, python3-usb, symlinks
hdmi-switch list && hdmi-switch 1
```

Needs only `python3`, `python3-usb` (pyusb) and `libusb-1.0`. Works offline.

## Use

```
hdmi-switch 3                 # select HDMI input 3        (also: next, list, send 0xNN)
irkbd.py --node 3 type "ls -la\n"
irkbd.py --node 3 key ctrl-alt-t
irkbd.py --node 3 release
kvm 3 type "hello\n"          # switch to input 3 and type on node 3
```

Convention: HDMI input *N* carries the machine whose XIAO has node id *N*. Node 0 is broadcast.
`irkbd.py` uses the US keyboard layout; `\n` = Enter, `\t` = Tab. Chords accept
`ctrl shift alt gui` (+ `r`-prefixed right-hand versions) and key names like `enter esc f5 up home`.

## Flash a receiver

Uses [arduino-cli](https://arduino.github.io/arduino-cli/) with the `esp32:esp32` core.

```
cd irkbd-firmware
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default --output-dir build .
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32S3:USBMode=default,CDCOnBoot=default -p /dev/ttyACM0 --input-dir build .
```

The first upload attempt often fails while the board re-enumerates into its bootloader; just run it
again. Then set the node id over the board's serial port (it keeps it in flash):

```
$ python3 -m serial.tools.miniterm /dev/ttyACM0 115200
id 3          -> node id = 3 (saved)
stat          -> pin=1 edges=... frames=... crcerr=...   (bring-up diagnostics)
raw on        -> dumps every pulse/space and decoder state transition
```

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

### Dongle quirks worth knowing

- It buffers about 392 bytes of pulse data (7 × 56-byte fragments); longer frames are truncated.
  Hence 16-character string frames.
- It acknowledges a transmit slightly *before* the last pulses leave. Sending the next frame
  immediately corrupts it; waiting 0.1 × the frame's air time is enough (`irkbd.py` does this).
- Its IR *receiver* was unreliable in testing (one capture in many tries), so the HDMI switch codes
  were found by probing with `irprobe.py` (a small Tk app: step through codes, label what works).
- The carrier frequency argument is ignored by the hardware (fixed ~36.6 kHz).

## Radios

The XIAO ESP32S3 has WiFi and Bluetooth. This firmware never initialises either, but if you want a
receiver with no radio at all, the XIAO RP2040 has the same footprint and pinout and native USB HID.
A port is planned.

## Files

| File | What |
|---|---|
| `hdmi-switch` | CLI for the HDMI switch (`codes.json`) |
| `irkbd.py` | IR keyboard bus sender |
| `kvm` | `hdmi-switch` + `irkbd.py` in one call |
| `irkbd-firmware/` | XIAO ESP32S3 receiver firmware (Arduino) |
| `irsend.py` | send one NEC frame (addr, cmd) |
| `irprobe.py` | Tk helper for discovering a device's IR codes |
| `tiqiaa_usb_ir.py` | Tiqiaa dongle driver (third-party, see NOTICE) |
| `99-tiqiaa-ir.rules` | udev rule (group `plugdev`) |
| `install-control-node.sh` | installer for the control machine |

## License

Apache-2.0 (see `LICENSE`). `tiqiaa_usb_ir.py` is by Norman Rasmussen, building on Xen (XenRE)'s
reverse engineering; see `NOTICE`.
