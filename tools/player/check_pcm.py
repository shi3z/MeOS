import array,json,sys
from pathlib import Path
root=Path(sys.argv[1]);fixtures=Path(__file__).parent/'fixtures'
for row in json.loads((fixtures/'manifest.json').read_text()):
 samples=array.array('h',(root/(row['file']+'.pcm')).read_bytes())
 if sys.byteorder!='little':samples.byteswap()
 for channel in [0,1]:
  signal=samples[channel::2][6000:10800]
  peak=max(abs(x) for x in signal);assert 1000<peak<20000,(row,peak)
  crossings=sum(a<=0<b for a,b in zip(signal,signal[1:]))
  frequency=crossings*24000/(len(signal)-1)
  wanted=880 if row['channels']==2 and channel==1 else 440
  assert abs(frequency-wanted)<12,(row,channel,frequency)
 if row['channels']==1:assert samples[0::2]==samples[1::2],row
print('PCM: all source rates, 440/880Hz channel identity, amplitude, mono duplication PASS')
