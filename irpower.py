#!/usr/bin/env python3
"""irpower: switch outlets on the Matter power strip through an ir-kvm power node (irpower-firmware).

  irpower --node 200 on                 all outlets on
  irpower --node 200 off --outlet 2     one outlet off
  irpower --node 200 toggle --outlet 4

Frame type 4 on the IRKB bus: dst, 4<<4|seq, outlet (0 = all), action (0 off, 1 on, 2 toggle), crc8.
The power node dedupes on seq, so --twice is safe and recommended: a lost frame here means a
machine stays dark. Transmitter selection as for irkbd (IRKVM_TX).
"""
import argparse, sys, os, time
sys.path.insert(0, os.path.dirname(os.path.realpath(__file__)))
from irkbd import Sender

ACTIONS = {'off': 0, 'on': 1, 'toggle': 2}

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--node', type=int, required=True, help='IR node id of the power node')
    ap.add_argument('--outlet', type=int, default=0, help='Matter endpoint 1..N of the outlet; 0 = all (default)')
    ap.add_argument('--twice', action='store_true', help='send each frame twice (deduped on the node)')
    ap.add_argument('--nec', action='store_true', help='classic NEC timing (slower, more robust)')
    ap.add_argument('action', choices=sorted(ACTIONS))
    a = ap.parse_args()
    s = Sender(node=a.node, times=2 if a.twice else 1, timing='nec' if a.nec else 'fast')
    s.chord(a.outlet, ACTIONS[a.action], ftype=4)
    print(f"sent power {a.action} outlet={a.outlet or 'all'} to node {a.node} via {s.tx.name}")

if __name__ == '__main__':
    main()
