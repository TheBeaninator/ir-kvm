"""irtx: one interface over the two IR transmitters ir-kvm supports.

  TiqiaaTx  — the Tiqiaa "Tview" USB dongle (10c4:8468), via tiqiaa_usb_ir.py
  NodeTx    — an ir-kvm XIAO RP2040 node with an IR LED, over its USB serial port

open_tx() picks one: $IRKVM_TX = "tiqiaa" | "/dev/ttyACM0" | "auto" (default). Auto prefers the
dongle if present, else the first serial node that answers "ping" with tx=1.

Both expose send(codes) where codes is a list of +mark/-space microseconds, and
send_bytes(data, fast) which builds the IRKB/NEC-style frame from bytes (nodes do the framing on
board, so the serial line stays short). airtime accounting is handled inside send().
"""
import glob, os, sys, time
HERE = os.path.dirname(os.path.realpath(__file__))
sys.path.insert(0, HERE)

TIMING = {'nec': (4500, 560, 560, 1690), 'fast': (2250, 350, 350, 1000)}

def frame_codes(data, fast=True):
    lead_space, mark, sp0, sp1 = TIMING['fast' if fast else 'nec']
    codes = [9000, -lead_space]
    for byte in data:
        for i in range(8):
            codes += [mark, -sp1 if (byte >> i) & 1 else -sp0]
    return codes + [mark, -5000]

class TiqiaaTx:
    name = 'tiqiaa'
    def __init__(self):
        import usb.core, tiqiaa_usb_ir as t
        dev = usb.core.find(idVendor=0x10c4, idProduct=0x8468)
        if dev is None: raise FileNotFoundError('Tiqiaa dongle 10c4:8468 not found')
        self.t, self.dev = t, t.TiqiaaUsbIr(dev)
    def send(self, codes):
        air = sum(abs(c) for c in codes) / 1e6
        self.dev.SendIR(38000, self.t.TiqiaaIrSignal(codes).ToTiqiaa())
        time.sleep(0.02 + 0.1 * air)   # it acks slightly before the last pulses are out
    def send_bytes(self, data, fast=True):
        self.send(frame_codes(data, fast))

class NodeTx:
    name = 'node'
    def __init__(self, port):
        import serial
        self.port = port
        self.s = serial.Serial(port, 115200, timeout=2)
        time.sleep(0.05); self.s.reset_input_buffer()
        info = self._cmd('ping')
        if not info.startswith('irkvm') or 'tx=1' not in info:
            raise RuntimeError(f'{port}: not an ir-kvm node with an IR LED ({info!r})')
        self.info = info
    def _cmd(self, line):
        self.s.write((line + '\n').encode()); return self.s.readline().decode(errors='replace').strip()
    def _ok(self, line):
        r = self._cmd(line)
        if r != 'ok': raise RuntimeError(f'node said {r!r} to {line[:40]!r}')
    def send(self, codes):
        self._ok('raw ' + ' '.join(str(c) for c in codes))
    def send_bytes(self, data, fast=True):
        if fast: self._ok('irkb ' + bytes(data).hex())
        else: self.send(frame_codes(data, fast=False))

def node_ports():
    return sorted(glob.glob('/dev/serial/by-id/*XIAO*') + glob.glob('/dev/serial/by-id/*Seeed*')) or sorted(glob.glob('/dev/ttyACM*'))

def open_tx(spec=None):
    spec = spec or os.environ.get('IRKVM_TX', 'auto')
    if spec == 'tiqiaa': return TiqiaaTx()
    if spec.startswith('/dev/'):
        try: return NodeTx(spec)
        except Exception as e: sys.exit(f'IRKVM_TX={spec}: {e}')
    try: return TiqiaaTx()
    except Exception: pass
    for p in node_ports():
        try: return NodeTx(p)
        except Exception: continue
    sys.exit('no IR transmitter: no Tiqiaa dongle (10c4:8468) and no ir-kvm node with an IR LED on a serial port (set IRKVM_TX)')
