#!/usr/bin/python3
# vim: ts=2 sw=2 sts=2 si

import argparse
import array
import atexit
import collections
import enum
import logging
import math
import os
import queue
import struct
import socket
import sys
import threading
import time
import typing
import usb.core
import usb.util


logger = logging.getLogger(__name__)


class NamedStructMeta(type):

  def __new__(mcs, typename, bases, ns):
    if '__annotations__' not in ns:
      return type.__new__(mcs, typename, bases, ns)

    annotations = ns['__annotations__']
    cls = collections.namedtuple(typename + 'NamedTuple', annotations)
    return type.__new__(mcs, typename, (cls,) + bases, dict(
      _struct=struct.Struct(''.join(annotations.values())),
    ))


class NamedStruct(metaclass=NamedStructMeta):

  _struct = ...  # struct.Struct

  def pack(self):
    return self._struct.pack(*self)

  @classmethod
  def unpack(cls, buf):
    return cls._make(cls._struct.unpack(buf))

  @classmethod
  @property
  def size(self):
    return self._struct.size


class Report2Header(NamedStruct):
  ReportId: 'B'
  FragmSize: 'B'
  PacketIdx: 'B'
  FragmCount: 'B'
  FragmIdx: 'B'


class CmdHeader(NamedStruct):
  CmdId: 'B'
  CmdType: 'c'


class VersionPacket(NamedStruct):
  VersionChar: 'c'
  VersionInt: 'B'
  VersionGuid: '36s'


MaxUsbFragmSize = 56
MaxUsbPacketSize = 1024
MaxUsbPacketIndex = 15
MaxCmdId = 0x7f
PacketStart = b'ST'
PacketEnd = b'EN'
ReadReportId = 1
WriteReportId = 2


class State(enum.Enum):
  Idle = 3
  Send = 9
  Recv = 19


class Command(enum.Enum):
  Unknown = b'H'
  Version = b'V'
  IdleMode = b'L'
  SendMode = b'S'
  RecvMode = b'R'
  Data = b'D'
  Output = b'O'
  Cancel = b'C'


class Error(Exception):
  pass


class TiqiaaUsbIr:

  def __init__(self, dev):

    self.dev = dev

    # set the active configuration. With no arguments, the first
    # configuration will be the active one
    #dev.set_configuration()

    # get an endpoint instance
    cfg = dev.get_active_configuration()
    intf = cfg[(0, 0)]

    self.rep = usb.util.find_descriptor(
        intf,
        # match the first IN endpoint
        custom_match = lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) ==
            usb.util.ENDPOINT_IN)

    self.wep = usb.util.find_descriptor(
        intf,
        # match the first OUT endpoint
        custom_match = lambda e:
            usb.util.endpoint_direction(e.bEndpointAddress) ==
            usb.util.ENDPOINT_OUT)

    assert self.rep is not None
    assert self.wep is not None

    self.cmd_id = 0
    self.packet_idx = 0
    self.replies = queue.Queue()
    self.active = True
    self.read_thread = threading.Thread(target=self.ReadThread, daemon=True)
    self.read_thread.start()
    atexit.register(self._Close)

  def _Close(self):
    if self.active:
      reply = self.SendCmdAndWaitReply(Command.IdleMode)
      if reply:
        logger.info(reply)
      self.active = False
    self.dev.finalize()
    self.read_thread.join()

  def NextID(self):
    if self.cmd_id < MaxCmdId:
      self.cmd_id += 1
    else:
      self.cmd_id = 1
    return self.cmd_id

  def SendReport(self, data: bytes):
    report = array.array('B')
    report.frombytes(PacketStart)
    report.frombytes(data)
    report.frombytes(PacketEnd)
    if self.packet_idx < MaxUsbPacketIndex:
      self.packet_idx += 1
    else:
      self.packet_idx = 1
    fragment_cnt = math.ceil(len(report) / MaxUsbFragmSize)
    fragment_idx = 0

    while report:
      fragment_idx += 1
      frag = report[:MaxUsbFragmSize]
      hdr = Report2Header(WriteReportId,
                          len(frag) + 3, self.packet_idx, fragment_cnt,
                          fragment_idx)
      buf = hdr.pack() + frag
      logger.debug('> ' + ' '.join('%02x' % x for x in buf))
      self.wep.write(buf)
      report = report[MaxUsbFragmSize:]

  def RecvPacket(self):
    packet = array.array('B')

    while True:
      buf = self.rep.read(100, timeout=None)
      if not buf:
        continue
      if len(buf) < Report2Header.size:
        logger.debug('< ' + ' '.join('%02x' % x for x in buf))
        # too short, not enough header
        raise Error('Short read')
      hdr = Report2Header.unpack(buf[:Report2Header.size])
      frag = buf[Report2Header.size:]
      if hdr.ReportId != ReadReportId:
        logger.debug('< ' + ' '.join('%02x' % x for x in buf))
        # wrong type
        raise Error('Unexpected ReportID')
      if hdr.FragmSize - 3 > len(frag):
        logger.debug('< ' + ' '.join('%02x' % x for x in buf))
        # reported length longer than read length
        raise Error('Size larger then read')
      packet.frombytes(frag[:hdr.FragmSize - 3])
      logger.debug(f'< ' + ' '.join('%02x' % x for x in buf[:hdr.FragmSize + 2]))
      if hdr.FragmIdx == hdr.FragmCount:
        break

    if packet[:2].tobytes() != PacketStart:
      # wrong start
      raise Error('Missing Start')
    if packet[-2:].tobytes() != PacketEnd:
      # wrong end
      raise Error('Missing End')
    return self.ProcessRecvPacket(packet[2:-2])

  def ProcessRecvPacket(self, data):
    cmd = CmdHeader.unpack(data[:CmdHeader.size])
    state = State(data[-1])
    data = data[CmdHeader.size:-1]
    # cmd.CmdId
    cmdType = Command(cmd.CmdType)
    logger.info(f'< {cmd.CmdId} {cmdType} {state}')
    if cmdType == Command.Version:
      return cmd.CmdId, VersionPacket.unpack(data)
    if cmdType == Command.Data:
      return cmd.CmdId, data
    if data:
      return cmd.CmdId, data
    return cmd.CmdId, None

  def ReadThread(self):
    while self.active:
      try:
        p = self.RecvPacket()
        self.replies.put(p)
      except Error as e:
        logger.error(f'Error: {e}')
      except usb.core.USBError as e:
        if not self.active:  # disconnect
          break
        if e.errno == 110:  # read timed out.
          continue
        self.active = False
        self.replies.put((None, e))
        break

  def SendCmdAndWaitReply(self, cmd_type: Command, cmd_data: bytes = b''):
    cmd_id = self.NextID()
    logger.info(f'> {cmd_id} {cmd_type} {len(cmd_data)}')
    self.SendReport(CmdHeader(cmd_id, cmd_type.value).pack() + cmd_data)
    rid, reply = self.replies.get()
    if isinstance(reply, Exception):
      raise reply
    if cmd_type != Command.Output:
      assert rid == cmd_id, cmd_type
    return reply

  def SendIR(self, freq: int, data: typing.List[int]):
    reply = self.SendCmdAndWaitReply(Command.SendMode)
    if reply:
      logger.info(reply)

    freq = 0  # TODO: add logic for carrier frequency
    cdata = freq.to_bytes(1, 'big') + b''.join(
        d.to_bytes(1, 'big') for d in data)
    return self.SendCmdAndWaitReply(Command.Data, cdata)

  def RecvIR(self):
    reply = self.SendCmdAndWaitReply(Command.RecvMode)
    if reply:
      logger.info(reply)
    while True:
      yield self.SendCmdAndWaitReply(Command.Output)


class IrSignal:

  codes = ...  # typing.List[μs]

  def __init__(self, codes):
    self.codes = codes

  @classmethod
  def FromIr(cls, line):
    return cls([int(c) for c in line.split('#')[0].split()])

  def ToIr(self):
    return ' '.join('%+d' % c for c in self.codes) + ' #'

  def ToMode2(self):
    return '\n'.join(('pulse %d' % c if c > 0 else 'space %d' % -c) for c in self.codes) + '\ntimeout 0'


class TiqiaaIrSignal(IrSignal):

  TickSize = 16  # μs
  PulseBit = 0x80
  PulseMask = 0xFF & ~PulseBit

  @classmethod
  def FromTiqiaa(cls, data: typing.List[int]):
    codes = []
    prev_lvl = None
    for d in data:
      lvl = d & cls.PulseBit
      d = d & cls.PulseMask
      if not lvl:
        d = -d
      # TODO: add carrier removal
      if prev_lvl != lvl:
        codes.append(d * cls.TickSize)
      else:
        codes[-1] += d * cls.TickSize
      prev_lvl = lvl
    return cls(codes)

  def ToTiqiaa(self):
    data = []
    for c in self.codes:
      # TODO: accumulate truncation error
      d = abs(int(c / self.TickSize))
      while d:
        b = min(d, self.PulseMask)
        d -= b
        if c > 0:
          b |= self.PulseBit
        data.append(b)
    return data


def configure_logging(verbosity):
  FORMAT = '%(levelname).1s %(asctime)-15.19s %(filename)s:%(lineno)d %(message)s'
  offset = (logging.INFO - logging.WARNING) * verbosity
  logging.basicConfig(format=FORMAT, level=logging.WARNING + offset)


def parse_args():
  parser = argparse.ArgumentParser()
  parser.add_argument(
      '-d',
      '--device',
      default='10c4:8468',
      help='device filename or vendor and product ID numbers (in hexadecimal)')
  parser.add_argument(
      '-q', '--quiet', action='count', default=0, help='quiet output')
  parser.add_argument(
      '-v', '--verbose', action='count', default=0, help='verbose output')
  parser.add_argument(
      '-V', '--version', action='store_true', help='print versions')
  parser.add_argument(
      '-r', '--receive', nargs='?', const=sys.stdout, type=argparse.FileType('wt'), help='receive IR to stdout')
  parser.add_argument(
      '-s', '--send', type=argparse.FileType('rt'), metavar='FILE', help='send IR pulse and space file')
  parser.add_argument(
      '-c', '--carrier', type=int, default=38000, help='set carrier frequency')
  parser.add_argument(
      '-R', '--reset', action='store_true', help='reset the device')
  parser.add_argument(
      '-1', '--one-shot', action='store_true', help='end receiving after first message')
  parser.add_argument(
      '--mode2', action='store_true', help='output in mode2 format')
  return parser.parse_args()


def main():
  args = parse_args()
  configure_logging(args.verbose - args.quiet)

  logger.info(f'device = find({args.device})')
  values = (int(x, 16) if x else None for x in args.device.split(':'))
  key_values = zip(['idVendor', 'idProduct'], values)
  kwargs = {key: val for key, val in key_values if val is not None}
  # find our device
  dev = usb.core.find(**kwargs)

  # was it found?
  if dev is None:
    raise SystemExit(f'Device not found: {args.device}')

  logger.info(repr(dev))

  if args.reset:
    dev.reset()

  dev = TiqiaaUsbIr(dev)

  if args.version:
    print(dev.SendCmdAndWaitReply(Command.Version))

  if args.receive:
    for data in dev.RecvIR():
      s = TiqiaaIrSignal.FromTiqiaa(data)
      print(s.ToMode2() if args.mode2 else s.ToIr(), file=args.receive, flush=True)
      if args.one_shot: break

  if args.send:
    for line in args.send:
      data = TiqiaaIrSignal.FromIr(line).ToTiqiaa()

      reply = dev.SendIR(args.carrier, data)
      if reply:
        logger.info(reply)


if __name__ == '__main__':
  main()
