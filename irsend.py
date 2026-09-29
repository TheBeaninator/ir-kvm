#!/usr/bin/env python3
"""Send an NEC IR code through the Tiqiaa Tview USB dongle (10c4:8468).

Usage: irsend.py ADDR CMD [--repeat N]     e.g. irsend.py 0x80 0x05
The dongle generates the ~36.6 kHz carrier itself; we send the on/off envelope
as bytes of 16 us ticks (bit7 = emitter on). NEC: 9 ms lead, 4.5 ms space,
then addr, ~addr, cmd, ~cmd LSB-first, bit = 560 us mark + 560/1690 us space.
"""
import argparse, os, sys, time, usb.core
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tiqiaa_usb_ir as t

def nec_envelope(addr, cmd):
    codes = [9000, -4500]
    for byte in (addr & 0xff, (~addr) & 0xff, cmd & 0xff, (~cmd) & 0xff):
        for i in range(8):
            codes += [560, -1690 if (byte >> i) & 1 else -560]
    codes += [560, -40000]  # final burst + gap
    return codes

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('addr', type=lambda x: int(x, 0))
    ap.add_argument('cmd', type=lambda x: int(x, 0))
    ap.add_argument('--repeat', type=int, default=1, help='send the frame N times')
    ap.add_argument('-v', action='count', default=0)
    a = ap.parse_args()
    t.configure_logging(a.v)
    dev = usb.core.find(idVendor=0x10c4, idProduct=0x8468)
    if dev is None:
        sys.exit('Tiqiaa dongle 10c4:8468 not found')
    d = t.TiqiaaUsbIr(dev)
    data = t.TiqiaaIrSignal(nec_envelope(a.addr, a.cmd)).ToTiqiaa()
    for _ in range(a.repeat):
        d.SendIR(38000, data)
        time.sleep(0.1)
    print(f'sent NEC addr=0x{a.addr:02X} cmd=0x{a.cmd:02X} x{a.repeat} ({len(data)} bytes)')

if __name__ == '__main__':
    main()
