#!/usr/bin/env python3
import argparse
import serial,time,re
parser=argparse.ArgumentParser(description="Read-only Tab5 MP3 controls and repeat-play smoke test; opening serial may reset the board")
parser.add_argument("port")
parser.add_argument("tracks",nargs="+",help="Absolute card MP3 paths; first runs controls and 64-second playback, others run 11 seconds")
parser.add_argument("--output",default="player-device.log")
args=parser.parse_args()
for path in args.tracks:
 if not path.startswith("/sd/") or any(c in path for c in '\r\n"'):
  parser.error('tracks must be /sd/ paths without newlines or double quotes')
s=serial.Serial(port=None,baudrate=115200,timeout=.05,exclusive=True)
s.dtr=False;s.rts=False;s.port=args.port;s.open()
f=open(args.output,"w")
def read(seconds):
 data=bytearray();end=time.monotonic()+seconds
 while time.monotonic()<end:data.extend(s.read(4096))
 text=data.decode(errors='replace').replace('\r','');f.write(text);f.flush();print(text,flush=True);return text

def send(text):
 print('SEND',repr(text),flush=True);s.write(text.encode());s.flush()
def command(text,seconds=2):send(text+'\r');return read(seconds)
def play(path):
 result=command('play "'+path+'"',5)
 assert re.search(r'mp3: \d+ Hz, [12] channel\(s\)',result),result

def stop():
 send('q');result=read(3)
 assert 'mp3: stopped (0)' in result and '0 underruns/errors' in result,result
 assert 'guard OK' in result,result
 return result
try:
 read(10);assert 'mounted /sd read-only' in command('sd mount',3)
 play(args.tracks[0])
 send(' ');paused=read(1);assert 'mp3: paused at ' in paused
 read(2)
 send('i');status1=read(1);send('i');status2=read(1)
 pattern=r'paused, played (\d+) frames'
 assert re.search(pattern,status1).group(1)==re.search(pattern,status2).group(1)
 send('--');vol=read(1);assert 'volume 40%' in vol
 send('+');vol=read(1);assert 'volume 45%' in vol
 send(' ');assert 'mp3: resumed at ' in read(1)
 for _ in range(4):
  read(15);send('i');status=read(1);assert 'underruns 0' in status
 stop();command('mem');command('apps -l')
 for name in args.tracks[1:]:
  play(name);read(6);stop();command('mem');command('apps -l')
 command('ps');command('sd status')
 print('DEVICE_CONTROLS_PASS',flush=True)
finally:s.close();f.close()
