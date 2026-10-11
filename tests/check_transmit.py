"""Independent crypto and coherent IQ checks of the actual packed TX waveform."""
import ctypes as C
import hashlib, hmac, struct, subprocess, tempfile, sys
from pathlib import Path
import numpy as np
from scipy import signal
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
ROOT=Path(__file__).resolve().parent.parent
sys.path.insert(0,str(ROOT/'tools'))
import lora_decode as phy
class Wave(C.Structure):
    _fields_=[('first',C.POINTER(C.c_uint8)),('second',C.POINTER(C.c_uint8)),('first_size',C.c_size_t),('second_size',C.c_size_t),('samples',C.c_uint32),('position',C.c_uint32),('quantum',C.c_uint),('cancelled',C.c_void_p),('context',C.c_void_p),('extra',C.POINTER(C.c_uint8)),('extra_size',C.c_size_t)]
with tempfile.TemporaryDirectory() as tmp:
    so=Path(tmp)/'tx.so'
    subprocess.run(['cc','-shared','-fPIC','-O2','-Wall','-Wextra','-Werror',*[str(ROOT/'app'/s) for s in ['transmit_wave.c','transmit_packet.c','protocol.c','mc_public.c','identity_name.c']],'-o',str(so)],check=True)
    lib=C.CDLL(str(so));lib.lora_tx_packet.restype=C.c_size_t;lib.lora_tx_waveform.restype=C.c_bool
    lib.lora_tx_message.restype=C.c_size_t
    cases=[(i,None) for i in range(4)]+[(a,b'Hello mesh!') for a in (0,2)]+[(0,b'M'*34),(2,b'T'*44)]
    for action,message in cases:
        out=(C.c_uint8*64)()
        n=lib.lora_tx_packet(out,action,17,1791754245,0xf11f1234,True,2,b'Flipper') if message is None else lib.lora_tx_message(out,action==2,message,17,1791754245,0xf11f1234,True,2,b'Flipper')
        data=bytes(out[:n]);assert n
        if action==0:
            key=bytes.fromhex('8b3387e9c5cdea6ac9e5edbaa115cd72')
            assert data[:3]==bytes.fromhex('154011') and data[3:5]==hmac.digest(key,data[5:],'sha256')[:2]
            dec=Cipher(algorithms.AES(key),modes.ECB()).decryptor();plain=dec.update(data[5:])+dec.finalize()
            assert plain[5:].rstrip(b'\0')==(b'Flipper: TEST 17' if message is None else b'Flipper: '+message)
        elif action==1:assert data==bytes.fromhex('2e4080041100000000000000')
        else:
            key=bytes.fromhex('d4f1bb3a20290759f0bcffabcf4e6901')
            nonce=struct.pack('<QI',17,0xf11f1234)+bytes(4)
            dec=Cipher(algorithms.AES(key),modes.CTR(nonce)).decryptor();plain=dec.update(data[16:])+dec.finalize()
            if action==2:
                expected=message if message is not None else b'Flipper TEST 17'
                assert plain==b'\x08\x01\x12'+bytes([len(expected)])+expected
            else:assert plain==b'\x08\x04\x12\x1c\x0a\x09!f11f1234\x12\x07Flipper\x1a\x041234\x48\x01\x18\x01' and data[12]==0
        a=(C.c_uint8*16384)();b=(C.c_uint8*26752)();w=Wave(a,b,len(a),len(b))
        extra=(C.c_uint8*2048)()
        if action>=2 and n>48:w.extra=extra;w.extra_size=len(extra)
        assert lib.lora_tx_waveform(C.byref(w),out,n,action>=2)
        packed=bytes(a)+bytes(b)+(bytes(extra) if w.extra_size else b'')
        levels=np.repeat(np.unpackbits(np.frombuffer(packed,dtype=np.uint8),bitorder='little')[:w.samples],w.quantum).astype(float)
        sf,bw,pre=(11,250000,16) if action>=2 else (7,62500,32)
        chips=1<<sf;period=chips*1000000//bw
        z=signal.fftconvolve(levels-.5,signal.firwin(513,140000,fs=1000000),'same').astype(complex) if action>=2 else signal.hilbert(signal.fftconvolve(levels,signal.firwin(513,[7000,bw+14000],pass_zero=False,fs=1000000),'same'))
        start=int((pre+4.25)*period)
        def sym(i):
            t=np.arange(chips)*period/chips
            at=start+i*period+t
            v=np.interp(at,np.arange(len(z)),z.real)+1j*np.interp(at,np.arange(len(z)),z.imag)
            ref=np.exp(-2j*np.pi*((-125000 if action>=2 else 10000)*t+bw*t*t/(2*period))/1000000)
            return int(abs(np.fft.fft(v*ref)).argmax())
        hs=[sym(i) for i in range(8)]
        h=[phy.hamming_dec(c,4)[0] for c in phy.deinterleave([phy.gray(((x-1)%chips)//4) for x in hs],sf-2,8)]
        assert phy.header_check(h)==h[3]*16+h[4] and h[0]*16+h[1]==n,(action,hs,h,n)
        nibs=h[5:];bad=0
        count=((2*(n+2)-(sf-7)+sf-1)//sf)*5
        for i in range(0,count,5):
            syms=[sym(8+i+j) for j in range(5)]
            for cw in phy.deinterleave([phy.gray((x-1)%chips) for x in syms],sf,5):
                nib,ok=phy.hamming_dec(cw,1);nibs.append(nib);bad+=not ok
        got,crc=phy.assemble(nibs,n,True)
        assert bytes(got)==data and phy.lora_crc_ok(got,crc) and not bad,(action,bytes(got).hex(),data.hex(),bad)
        Path(tmp,f'wave{action}.bin').write_bytes(packed[:(w.samples+7)//8])
        print(f'PASS action {action} ({len(message) if message is not None else "test/query"}): independent crypto, real OOK IQ -> exact packet + PHY CRC; {len(levels)} us, {(w.samples+7)//8} bytes')
        small=Wave(a,b,1,0);assert not lib.lora_tx_waveform(C.byref(small),out,n,action>=2)
    # Width changes only MeshCore routing metadata, not encrypted payload.
    for action in range(4):
        for flood in (False,True):
            reference=None
            for width in (1,2,3):
                n=lib.lora_tx_packet(out,action,17,1791754245,0xf11f1234,flood,width,b'Flipper')
                packet=bytes(out[:n]);assert n
                if action < 2:
                    assert packet[1]==(width-1)<<6
                    stable=packet[:1]+packet[2:]
                else:stable=packet
                if reference is not None:assert stable==reference
                reference=stable
            for width in (0,4,255):
                assert lib.lora_tx_packet(out,action,17,1791754245,0xf11f1234,flood,width,b'Flipper')==0
    for mt in (False,True):
        for width in (1,2,3):
            n=lib.lora_tx_message(out,mt,b'Width test',17,1791754245,0xf11f1234,True,width,b'Flipper')
            assert n and (mt or out[1]==(width-1)<<6)
        for width in (0,4,255):
            assert lib.lora_tx_message(out,mt,b'Width test',17,1791754245,0xf11f1234,True,width,b'Flipper')==0
    print('PASS hop widths 1/2/3, local/flood, unchanged MT bytes, invalid widths rejected')
    for mt,limit in [(False,34),(True,44)]:
        for bad in (b'',b'X'*(limit+1)):
            assert lib.lora_tx_message(out,mt,bad,17,1791754245,0xf11f1234,True,2,b'Flipper')==0
    name=b'ABCDEFGHIJKLMNOP'
    for mt in (False,True):
        limit=44 if mt else 25
        assert lib.lora_tx_message(out,mt,b'X'*limit,17,1791754245,0xf11f1234,True,2,name)
        if not mt: assert not lib.lora_tx_message(out,mt,b'X'*(limit+1),17,1791754245,0xf11f1234,True,2,name)
    assert lib.lora_tx_packet(out,3,17,1791754245,0xf11f1234,True,2,name) <= 64
print('PASS blank/oversized messages and bounded buffer rejection; digital tests are not RF receiver validation')
