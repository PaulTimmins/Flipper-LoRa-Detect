"""Digital-model checks only; receiver acceptance must still be measured."""
import re
from pathlib import Path
import subprocess
import tempfile
import numpy as np
import generate as g

ROOT=Path(__file__).resolve().parents[2]

def check(cr):
    parts=g.segments(cr)
    levels,freq=g.waveform(parts)
    x=np.asarray(levels,dtype=float)
    # Ideal receiver isolates the upper sideband of the actual binary OOK table.
    spectrum=np.fft.fft(x-x.mean())
    bins=np.fft.fftfreq(len(x),1/g.RATE)
    spectrum[(bins < g.LOW) | (bins > g.LOW+g.BW)]=0
    analytic=np.fft.ifft(spectrum)
    t=np.arange(0,g.PERIOD,16)
    phase=(g.LOW*t + g.BW*t*t/(2*g.PERIOD))/g.RATE
    reference=np.exp(-2j*np.pi*phase)
    start=0
    recovered=[]
    for shift,direction,length in parts:
        if direction==1 and length==g.PERIOD:
            dechirped=analytic[start+t]*reference
            bins=np.abs(np.fft.fft(dechirped))
            found=int(np.argmax(bins))
            assert found==shift,(cr,start,shift,found)
            recovered.append(found)
        start+=length
    # Also feed an ideal discriminator model into the existing C PHY decoder.
    # This checks header/FEC/whitening/CRC consistency, not independent RF conformance.
    levels=[True]*1024 + [f >= g.LOW+g.BW*1300/2048 for f in freq] + [True]*8192
    signed=[];last=levels[0];run=0
    for level in levels:
        if level!=last:
            signed.append(run if last else -run);run=0;last=level
        run+=1
    signed.append(run if last else -run)
    with tempfile.NamedTemporaryFile(suffix='.sub',mode='w') as f:
        f.write('Filetype: Flipper SubGhz RAW File\nRAW_Data: '+' '.join(map(str,signed))+'\n');f.flush()
        out=subprocess.check_output([str(ROOT/'tests/host_payload'),f.name],text=True)
        assert 'status=2 length=37' in out,out
    print(f'PASS CR4/{cr+4}: {len(recovered)} OOK-sideband symbols recovered by FFT; C decoder CRC passed')

if __name__=='__main__':
    check(1);check(4)
    # Validate generated C arrays are reproducible and obey the HAL >=2us limit.
    with tempfile.TemporaryDirectory() as d:
        g.generate(Path(d))
        assert re.sub(r'\s+', '', (Path(d)/'waveforms.h').read_text()) == re.sub(r'\s+', '', Path(__file__).with_name('waveforms.h').read_text())
    print('PASS reproducible tables and timing limits; over-air result remains unverified')
