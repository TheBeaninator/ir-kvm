#!/usr/bin/env python3
"""Tk helper for probing NEC codes on the Tiqiaa Tview USB IR dongle.

Type a command (hex), Send it, or step through codes with Prev/Next.
When you see what a code does, type a label and press Save; labels go to
codes.json next to this script (used by hdmi-switch tooling).
"""
import json, os, sys, time, tkinter as tk
from tkinter import ttk
import usb.core
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tiqiaa_usb_ir as t
from irsend import nec_envelope
CODES = os.path.join(HERE, 'codes.json')

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title('IR probe (Tiqiaa Tview)')
        self.dev = None
        self.codes = json.load(open(CODES)) if os.path.exists(CODES) else {'addr': '0x80', 'buttons': {}}
        f = ttk.Frame(self, padding=10); f.grid()
        ttk.Label(f, text='Addr').grid(row=0, column=0); self.addr = tk.StringVar(value=self.codes.get('addr', '0x80'))
        ttk.Entry(f, textvariable=self.addr, width=6).grid(row=0, column=1)
        ttk.Label(f, text='Cmd').grid(row=0, column=2); self.cmd = tk.StringVar(value='0x15')
        e = ttk.Entry(f, textvariable=self.cmd, width=6); e.grid(row=0, column=3); e.bind('<Return>', lambda _: self.send())
        ttk.Button(f, text='Send', command=self.send).grid(row=0, column=4)
        ttk.Button(f, text='< Prev', command=lambda: self.step(-1)).grid(row=1, column=2)
        ttk.Button(f, text='Next >', command=lambda: self.step(1)).grid(row=1, column=3)
        ttk.Label(f, text='Repeat').grid(row=1, column=0); self.rep = tk.IntVar(value=2)
        ttk.Spinbox(f, from_=1, to=5, textvariable=self.rep, width=4).grid(row=1, column=1)
        ttk.Label(f, text='Label this cmd as').grid(row=2, column=0, columnspan=2); self.label = tk.StringVar()
        le = ttk.Entry(f, textvariable=self.label, width=10); le.grid(row=2, column=2, columnspan=2); le.bind('<Return>', lambda _: self.save())
        ttk.Button(f, text='Save', command=self.save).grid(row=2, column=4)
        self.known = ttk.Frame(f); self.known.grid(row=3, column=0, columnspan=5, pady=6)
        self.log = tk.Text(f, width=48, height=12); self.log.grid(row=4, column=0, columnspan=5)
        self.refresh_known()

    def open(self):
        if self.dev is None:
            d = usb.core.find(idVendor=0x10c4, idProduct=0x8468)
            if d is None: raise RuntimeError('dongle 10c4:8468 not found')
            self.dev = t.TiqiaaUsbIr(d)
        return self.dev

    def out(self, s):
        self.log.insert('end', time.strftime('%H:%M:%S ') + s + '\n'); self.log.see('end')

    def send(self, cmd=None):
        try:
            a = int(self.addr.get(), 0); c = int(self.cmd.get(), 0) if cmd is None else cmd
            data = t.TiqiaaIrSignal(nec_envelope(a, c)).ToTiqiaa()
            for _ in range(self.rep.get()):
                self.open().SendIR(38000, data); time.sleep(0.1)
            self.out(f'sent addr=0x{a:02X} cmd=0x{c:02X}')
        except Exception as e:
            self.out(f'ERROR {e}'); self.dev = None

    def step(self, d):
        c = (int(self.cmd.get(), 0) + d) & 0xff
        self.cmd.set(f'0x{c:02X}'); self.send(c)

    def save(self):
        name = self.label.get().strip()
        if not name: return
        self.codes['addr'] = self.addr.get(); self.codes['buttons'][name] = self.cmd.get()
        json.dump(self.codes, open(CODES, 'w'), indent=2)
        self.out(f'saved {name} = {self.cmd.get()}'); self.label.set(''); self.refresh_known()

    def refresh_known(self):
        for w in self.known.winfo_children(): w.destroy()
        for i, (name, c) in enumerate(sorted(self.codes['buttons'].items())):
            ttk.Button(self.known, text=f'{name} ({c})', command=lambda c=c: (self.cmd.set(c), self.send())).grid(row=0, column=i)

if __name__ == '__main__':
    App().mainloop()
