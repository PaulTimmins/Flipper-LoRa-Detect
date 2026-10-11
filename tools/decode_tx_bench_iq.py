#!/usr/bin/env python3
"""Decode experimental MC upper-sideband or MT centered chirps from RTL-SDR IQ.

Timing comes from the recorded preamble and sync symbols; packet length and
coding rate come from the received header. Expected bytes are used ONLY for the
final comparison. No symbol substitution, CRC search or payload-assisted tuning.
Requires NumPy, SciPy and cryptography; reuses the project's PHY bit decoder.
"""
import argparse
import hashlib
import hmac
import json
from pathlib import Path

import numpy as np
from scipy import signal
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import lora_decode as phy


def decode(path, center, sample_rate, window, expected=None, meshtastic=False):
    sf,bw,syncs,preamble=(11,250000,[16,88],16) if meshtastic else (7,62500,[8,16],32)
    chips=1<<sf
    raw=np.fromfile(path,np.uint8)
    if len(raw)%2: raise ValueError('Truncated IQ pair')
    a,b=[int(t*sample_rate) for t in window]
    r=raw[2*a:2*b]
    x=(r[0::2].astype(float)-127.5)+1j*(r[1::2].astype(float)-127.5)
    nfft=1 << (len(x)*2-1).bit_length()
    spectrum=abs(np.fft.fft(x*np.hanning(len(x)),nfft))
    frequencies=np.fft.fftfreq(nfft,1/sample_rate)
    nominal=(906875000 if meshtastic else 910483612)-center
    candidates=abs(frequencies-nominal)<25000
    carrier=float(frequencies[np.argmax(np.where(candidates,spectrum,0))])
    mixed=x*np.exp(-2j*np.pi*carrier*np.arange(len(x))/sample_rate)
    # MC selects its upper sideband; centered MT retains both conjugate chirps.
    z=signal.fftconvolve(mixed,signal.firwin(513,140000,fs=sample_rate) if meshtastic else signal.firwin(513,[7000,bw+14000],pass_zero=False,fs=sample_rate),'same')
    period=sample_rate*chips/bw
    t=np.arange(round(period))
    reference=np.exp(2j*np.pi*((-125000 if meshtastic else 10000)*t+bw*t*t/(2*period))/sample_rate)
    correlation=abs(signal.fftconvolve(z,np.conj(reference[::-1]),'valid'))
    peaks,_=signal.find_peaks(correlation,distance=int(.95*period),height=correlation.max()*.5)
    groups=np.split(peaks,np.where(abs(np.diff(peaks)-period)>period*.005)[0]+1)
    pre=max(groups,key=len)
    if len(pre)<8: raise ValueError('No stable preamble')
    period,start=np.polyfit(np.arange(len(pre)),pre,1)
    grid=np.arange(len(z))
    def symbol(at):
        t=np.arange(chips)*period/chips
        positions=at+t
        if positions[-1]>=len(z): raise ValueError('Packet extends beyond window')
        samples=np.interp(positions,grid,z.real)+1j*np.interp(positions,grid,z.imag)
        ref=np.exp(-2j*np.pi*((-125000 if meshtastic else 10000)*t+bw*t*t/(2*period))/sample_rate)
        power=abs(np.fft.fft(samples*ref))
        i=int(np.argmax(power))
        return i,float(power[i]/np.partition(power,-2)[-2])
    sync=None
    for k in range(max(0,len(pre)-2),len(pre)+3):
        if [symbol(start+(k+j)*period)[0] for j in (-1,0,1)]==[0]+syncs:
            sync=k;break
    if sync is None: raise ValueError('LoRa sync symbols not found')
    header_start=start+(sync+4.25)*period
    hs=[symbol(header_start+k*period)[0] for k in range(8)]
    header_words=phy.deinterleave([phy.gray(((s-1)%chips)//4) for s in hs],sf-2,8)
    header=[phy.hamming_dec(c,4)[0] for c in header_words]
    if phy.header_check(header)!=header[3]*16+header[4]: raise ValueError('Header checksum failed')
    length=header[0]*16+header[1];cr=header[2]>>1;has_crc=bool(header[2]&1)
    if not 1<=cr<=4 or not has_crc: raise ValueError('Unexpected coding rate / missing CRC')
    blocks=(2*(length+2)-(sf-7)+sf-1)//sf
    count=blocks*(4+cr)
    recovered=[symbol(header_start+(8+k)*period) for k in range(count)]
    nibbles=header[5:];parity_bad=0
    for k in range(0,count,4+cr):
        words=phy.deinterleave([phy.gray((s-1)%chips) for s,_ in recovered[k:k+4+cr]],sf,4+cr)
        for word in words:
            n,ok=phy.hamming_dec(word,cr);nibbles.append(n);parity_bad+=not ok
    data,crc=phy.assemble(nibbles,length,has_crc)
    data=bytes(data)
    result=dict(recording=str(path),recording_sha256=hashlib.sha256(raw.tobytes()).hexdigest(),
        sample_rate=sample_rate,receiver_center_hz=center,analysis_window_seconds=window,
        observed_carrier_hz=center+carrier,frequency_axis_calibrated=False,
        first_preamble_seconds=(a+start)/sample_rate,preamble_symbols_used=len(pre),
        symbol_period_us=float(period/sample_rate*1e6),sync_symbols=syncs,
        header_symbols=hs,header_checksum_ok=True,payload_length=length,coding_rate=f'4/{4+cr}',
        payload_symbols=[s for s,_ in recovered],payload_parity_errors=parity_bad,
        minimum_symbol_peak_ratio=min(q for _,q in recovered),
        payload_hex=data.hex(),received_crc_hex=bytes(crc).hex(),
        payload_crc_ok=phy.lora_crc_ok(list(data),crc),public_mac_ok=False)
    # Independently check/decrypt the received group text; don't use expected data.
    key=bytes.fromhex('8b3387e9c5cdea6ac9e5edbaa115cd72')
    if len(data)>5 and data[0] in (0x15,0x16) and data[1]==0 and data[2]==hashlib.sha256(key).digest()[0]:
        cipher=data[5:]
        if len(cipher)%16==0 and hmac.compare_digest(data[3:5],hmac.digest(key,cipher,'sha256')[:2]):
            dec=Cipher(algorithms.AES(key),modes.ECB()).decryptor()
            plain=dec.update(cipher)+dec.finalize()
            result.update(public_mac_ok=True,chat_text=plain[5:].rstrip(b'\0').decode('utf-8'),
                          chat_timestamp=int.from_bytes(plain[:4],'little'))
    if meshtastic and len(data)>=16 and data[13]==8 and result['payload_crc_ok']:
        key=bytes.fromhex('d4f1bb3a20290759f0bcffabcf4e6901')
        nonce=data[8:12]+bytes(4)+data[4:8]+bytes(4)
        dec=Cipher(algorithms.AES(key),modes.CTR(nonce)).decryptor()
        plain=dec.update(data[16:])+dec.finalize()
        result['longfast_plain_hex']=plain.hex()
        if plain[:3]==b'\x08\x01\x12' and len(plain)>=4 and plain[3]<=len(plain)-4:
            result['chat_text']=plain[4:4+plain[3]].decode('utf-8')
    if expected is not None: result['exact_expected_match']=data==expected
    return result


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('recording',type=Path)
    parser.add_argument('--meshtastic',action='store_true')
    parser.add_argument('--center',type=float,default=910250000)
    parser.add_argument('--sample-rate',type=float,default=1000000)
    parser.add_argument('--window',type=float,nargs=2,default=[1.3,2.3])
    parser.add_argument('--expected',type=Path,help='waveforms.json; final comparison only')
    parser.add_argument('--output',type=Path)
    args=parser.parse_args()
    expected=bytes.fromhex(json.loads(args.expected.read_text())['payload_hex']) if args.expected else None
    result=decode(args.recording,args.center,args.sample_rate,args.window,expected,args.meshtastic)
    output=json.dumps(result,indent=2)
    if args.output: args.output.write_text(output+'\n')
    print(output)
    if not result['payload_crc_ok']: raise SystemExit(1)
