#!/usr/bin/env python3
r"""irkbd: type on a machine that has an IR-keyboard XIAO plugged in, via the Tiqiaa dongle.

  irkbd type "ls -la\n"        type text (US layout; \n = Enter, \t = Tab)
  irkbd key ctrl-alt-t          one chord: modifiers ctrl/shift/alt/gui + key name
  irkbd key enter | esc | f5 | up | ...
  irkbd raw MODS KEYCODE        send a raw HID chord

IRKB bus frame, NEC timing (9 ms lead, 4.5 ms space, bit = 560 us mark + 560/1690 us space),
5 bytes LSB-first per byte, i.e. one shared IR "layer 2" bus with addressed receivers:
  b0 = dst node id (0 = broadcast, 1..254; each XIAO has one, set over its serial port)
  b1 = type(4, high nibble) | seq(4)   type 0 = chord press+release, 1 = press/hold, 2 = release all,
                                       3 = string (b2 = count n, then n bytes: HID key | shift<<7)
  b2 = HID modifier mask    b3 = HID keycode          (types 0-2; frame is 5 bytes)
  last byte = CRC-8 (poly 0x07, init 0) over everything before it
Frames are sent once by default (--twice to double up); a receiver drops a repeat of the last seq.
Bit timing: fast (350/350/1000 us, lead space 2250) by default, --nec for classic NEC (lead space 4500).
Throughput ~70 chars/s in fast mode (16-char string frames; the dongle buffers ~392 bytes of pulses).
Node selection: --node N or env IRKBD_NODE (default 0 = broadcast).
Transmitter: the Tiqiaa dongle or an ir-kvm node with an IR LED, see irtx.py ($IRKVM_TX).
"""
import argparse, os, sys, time
HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)

MOD = {'ctrl': 0x01, 'shift': 0x02, 'alt': 0x04, 'gui': 0x08, 'super': 0x08, 'win': 0x08, 'meta': 0x08,
       'rctrl': 0x10, 'rshift': 0x20, 'ralt': 0x40, 'altgr': 0x40, 'rgui': 0x80}
# HID usage IDs (keyboard page)
KEYS = {'enter': 0x28, 'return': 0x28, 'esc': 0x29, 'escape': 0x29, 'backspace': 0x2A, 'tab': 0x2B, 'space': 0x2C,
        'capslock': 0x39, 'printscreen': 0x46, 'scrolllock': 0x47, 'pause': 0x48, 'insert': 0x49, 'home': 0x4A,
        'pageup': 0x4B, 'delete': 0x4C, 'del': 0x4C, 'end': 0x4D, 'pagedown': 0x4E, 'right': 0x4F, 'left': 0x50,
        'down': 0x51, 'up': 0x52, 'numlock': 0x53, 'menu': 0x65}
KEYS.update({f'f{i}': 0x3A + i - 1 for i in range(1, 13)})
KEYS.update({f'f{i}': 0x68 + i - 13 for i in range(13, 25)})
# ASCII -> (keycode, needs_shift)
ASCII = {}
for i, ch in enumerate('abcdefghijklmnopqrstuvwxyz'):
    ASCII[ch] = (0x04 + i, False); ASCII[ch.upper()] = (0x04 + i, True)
for i, ch in enumerate('1234567890'):
    ASCII[ch] = (0x1E + i, False)
for ch, code in zip('!@#$%^&*()', range(0x1E, 0x28)):
    ASCII[ch] = (code, True)
for lo, hi, code in [('\n', None, 0x28), ('\x1b', None, 0x29), ('\b', None, 0x2A), ('\t', None, 0x2B), (' ', None, 0x2C),
                     ('-', '_', 0x2D), ('=', '+', 0x2E), ('[', '{', 0x2F), (']', '}', 0x30), ('\\', '|', 0x31),
                     (';', ':', 0x33), ("'", '"', 0x34), ('`', '~', 0x35), (',', '<', 0x36), ('.', '>', 0x37), ('/', '?', 0x38)]:
    ASCII[lo] = (code, False)
    if hi: ASCII[hi] = (code, True)

def crc8(data, poly=0x07):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = ((c << 1) ^ poly) & 0xFF if c & 0x80 else (c << 1) & 0xFF
    return c

STRING_CHUNK = 16   # chars per string frame: the dongle buffers ~392 bytes of pulse data (7×56), a 20-byte frame is ~323

from irtx import open_tx, frame_codes, TIMING

def nec_bits(b, timing='fast'):
    return frame_codes(b, fast=(timing == 'fast'))

class Sender:
    def __init__(self, node=0, times=1, timing='fast', tx=None):
        self.node = node; self.timing = timing
        self.tx = tx or open_tx()
        self.seq = int(time.time()) & 0xF  # start pseudo-random so a restart doesn't collide with the last seq
        self.times = times
    def _send(self, body):
        frame = body + bytes([crc8(body)])
        for _ in range(self.times):
            self.tx.send_bytes(frame, fast=(self.timing == 'fast'))
    def _hdr(self, ftype):
        self.seq = (self.seq + 1) & 0xF
        return bytes([self.node & 0xFF, ((ftype & 0xF) << 4) | self.seq])
    def chord(self, mods, key, ftype=0):
        self._send(self._hdr(ftype) + bytes([mods & 0xFF, key & 0xFF]))
    def text(self, s):
        keys = []
        for ch in s:
            if ch not in ASCII: raise SystemExit(f'cannot type {ch!r} (US ASCII only)')
            code, shift = ASCII[ch]
            keys.append(code | (0x80 if shift else 0))
        for i in range(0, len(keys), STRING_CHUNK):
            chunk = keys[i:i + STRING_CHUNK]
            self._send(self._hdr(3) + bytes([len(chunk)]) + bytes(chunk))

def parse_chord(spec):
    mods, key = 0, None
    for part in spec.lower().split('-') if spec != '-' else ['-']:
        if part in MOD: mods |= MOD[part]
        elif part in KEYS: key = KEYS[part]
        elif len(part) == 1 and part in ASCII:
            key, shift = ASCII[part]; mods |= 0x02 if shift else 0
        else: raise SystemExit(f'unknown key {part!r}')
    if key is None: raise SystemExit('chord needs a key, e.g. ctrl-alt-t')
    return mods, key

def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--node', '-n', type=int, default=int(os.environ.get('IRKBD_NODE', '0')), help='destination node id (0 = all)')
    ap.add_argument('--twice', action='store_true', help='send each frame twice (default once)')
    ap.add_argument('--nec', action='store_true', help='use classic NEC bit timing instead of fast timing')
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('type').add_argument('text')
    sub.add_parser('key').add_argument('chord')
    sub.add_parser('release', help='release all keys on the node')
    r = sub.add_parser('raw'); r.add_argument('mods', type=lambda x: int(x, 0)); r.add_argument('key', type=lambda x: int(x, 0))
    a = ap.parse_args()
    s = Sender(node=a.node, times=2 if a.twice else 1, timing='nec' if a.nec else 'fast')
    if a.cmd == 'type':
        s.text(a.text.encode().decode('unicode_escape')); print(f'typed {len(a.text)} chars')
    elif a.cmd == 'release':
        s.chord(0, 0, ftype=2); print('released')
    elif a.cmd == 'key':
        s.chord(*parse_chord(a.chord)); print(f'sent {a.chord}')
    else:
        s.chord(a.mods, a.key); print(f'sent raw mods=0x{a.mods:02X} key=0x{a.key:02X}')

if __name__ == '__main__':
    main()
