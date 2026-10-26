#!/usr/bin/env python3
"""Exclusive serial session. No implicit reset or media write. Commands supplied explicitly.
--reset performs EN reset and discards RAM. A format command is destructive.
"""
import argparse,pathlib,serial,sys,time
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('--port',default='/dev/cu.usbmodem101');p.add_argument('--log',type=pathlib.Path,required=True)
p.add_argument('--reset',action='store_true');p.add_argument('--timeout',type=float,default=90)
p.add_argument('commands',nargs='*');a=p.parse_args()
s=serial.Serial(port=None,baudrate=115200,timeout=.1,exclusive=True);s.dtr=False;s.rts=False;s.port=a.port
with a.log.open('xb') as log:
 s.open()
 def collect(seconds,prompt=False):
  out=b'';end=time.monotonic()+seconds
  while time.monotonic()<end:
   b=s.read(4096)
   if b:out+=b;log.write(b);log.flush();sys.stdout.buffer.write(b);sys.stdout.buffer.flush()
   if prompt and (out.endswith(b'> ') or b'\n> ' in out):break
  return out
 try:
  if a.reset:
   sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]));from board_reset import reset_board
   reset_board(s);collect(a.timeout,True)
  else:collect(a.timeout,True)
  for command in a.commands:
   if command.startswith('@wait:'):
    collect(min(60,float(command.split(':',1)[1])));continue
   log.write(('\nSEND '+command+'\n').encode());s.write((command+'\r').encode());s.flush();collect(a.timeout,True)
 finally:s.close()
