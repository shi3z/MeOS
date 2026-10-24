#!/usr/bin/env python3
"""Run the real Tab5 VFS benchmark. Writes only its fresh MEOSBENCH directory.
Opening USB serial may reset the board. Firmware must already contain sd bench.
"""
import argparse,json,pathlib,re,time
import serial
p=argparse.ArgumentParser(description=__doc__)
p.add_argument('port');p.add_argument('--files',type=int,default=128)
p.add_argument('--small-bytes',type=int,default=1024);p.add_argument('--large-mib',type=int,default=8)
p.add_argument('--output',type=pathlib.Path,required=True)
a=p.parse_args()
if not (1<=a.files<=512 and 1<=a.small_bytes<=65536 and 1<=a.large_mib<=64):p.error('workload outside firmware limits')
s=serial.Serial(port=None,baudrate=115200,timeout=.1,exclusive=True)
s.dtr=False;s.rts=False;s.port=a.port
with a.output.open('x') as log:
    s.open()
    def collect(seconds,until=None):
        end=time.monotonic()+seconds;result=''
        while time.monotonic()<end:
            data=s.read(4096).decode(errors='replace').replace('\r','')
            if data:log.write(data);log.flush();result+=data;print(data,end='',flush=True)
            if until and until in result:return result
        return result
    def command(text,wait=5):
        log.write('\nSEND '+text+'\n');log.flush();s.write((text+'\r').encode());s.flush()
        return collect(wait)
    started=False
    try:
        collect(10);state=command('sd status',2)
        if 'FAT: mounted' in state and 'not mounted' not in state:raise RuntimeError('card already mounted; close apps and unmount before running benchmark')
        mounted=command('sd mount',5)
        if 'mounted /sd read/write' not in mounted:raise RuntimeError('read/write mount failed')
        s.write(f'sd bench {a.files} {a.small_bytes} {a.large_mib}\r'.encode());s.flush()
        started=True
        # All five phases are bounded in firmware; retain complete output even on failure.
        text='';end=time.monotonic()+3300
        while time.monotonic()<end:
            text+=collect(10,until='SDBENCH PASS')
            if 'SDBENCH PASS' in text or 'SDBENCH FAIL' in text or 'sd bench: failed' in text:break
        collect(1)
        command('sd unmount',3)
        started=False
        command('mem',2);command('ps',2);command('sd status',2)
        if 'SDBENCH PASS verified=1 cleaned=1' not in text:raise RuntimeError('benchmark failed; see retained directory in log')
        phases=[]
        for m in re.finditer(r'SDBENCH phase=(\S+) files=(\d+) bytes=(\d+) ms=(\d+) KiB_s=([\d.]+) files_s=([\d.]+) verified=(\d+)',text):
            phases.append(dict(phase=m[1],files=int(m[2]),bytes=int(m[3]),ms=int(m[4]),KiB_s=float(m[5]),files_s=float(m[6]),verified=bool(int(m[7]))))
        assert [r['phase'] for r in phases]==['small-write','small-read','large-write','large-read','small-delete'],phases
        assert phases[0]['bytes']==phases[1]['bytes']==a.files*a.small_bytes
        assert phases[2]['bytes']==phases[3]['bytes']==a.large_mib*1024*1024
        assert phases[1]['verified'] and phases[3]['verified']
        result=dict(files=a.files,small_bytes=a.small_bytes,large_MiB=a.large_mib,chunk=4096,
                    transport='bit-banged SPI, CMD17/CMD24',mode='VFS write-through; content verification included in reads',phases=phases)
        a.output.with_suffix('.json').write_text(json.dumps(result,indent=2)+'\n')
        print('DEVICE_BENCHMARK_PASS',flush=True)
    except BaseException:
        if started:
            try:
                s.write(b'\x03');s.flush();collect(3);command('sd unmount',3)
            except Exception:pass
        raise
    finally:s.close()
