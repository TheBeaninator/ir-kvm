#!/usr/bin/env python3
"""Send an NEC IR code through whichever IR transmitter is attached.

Usage: irsend.py ADDR CMD [--repeat N]     e.g. irsend.py 0x80 0x05
The dongle generates the ~36.6 kHz carrier itself; we send the on/off envelope
as bytes of 16 us ticks (bit7 = emitter on). NEC: 9 ms lead, 4.5 ms space,
then addr, ~addr, cmd, ~cmd LSB-first, bit = 560 us mark + 560/1690 us space.
"""
import argparse, os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

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
    a = ap.parse_args()
    from irtx import open_tx
    tx = open_tx()
    for _ in range(a.repeat):
        tx.send_nec(a.addr, a.cmd)
        time.sleep(0.05)
    print(f'sent NEC addr=0x{a.addr:02X} cmd=0x{a.cmd:02X} x{a.repeat} via {tx.name}')

if __name__ == '__main__':
    main()
