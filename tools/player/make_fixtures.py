"""Generate synthetic tones only; never copies user audio or opens a device."""
from pathlib import Path
import json,subprocess
out=Path(__file__).parent/'fixtures';out.mkdir(exist_ok=True)
rows=[]
for rate in [8000,11025,12000,16000,22050,24000,32000,44100,48000]:
 name=f'mono-{rate}.mp3'
 subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i',f'sine=frequency=440:sample_rate={rate}:duration=0.5','-ac','1','-c:a','libmp3lame','-b:a','32k' if rate<=24000 else '96k','-write_xing','0',str(out/name)],check=True)
 rows.append({'file':name,'rate':rate,'channels':1})
name='stereo-44100.mp3'
subprocess.run(['ffmpeg','-hide_banner','-loglevel','error','-y','-f','lavfi','-i','sine=frequency=440:sample_rate=44100:duration=0.5','-f','lavfi','-i','sine=frequency=880:sample_rate=44100:duration=0.5','-filter_complex','[0:a][1:a]amerge=inputs=2','-ac','2','-c:a','libmp3lame','-b:a','128k','-write_xing','0',str(out/name)],check=True)
rows.append({'file':name,'rate':44100,'channels':2})
(out/'manifest.json').write_text(json.dumps(rows,indent=2)+'\n')
