"""Check runtime crypto and pulse generation against independent Python code."""
import ctypes
import hmac
from pathlib import Path
import struct
import subprocess
import tempfile
import numpy as np
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import generate as reference

ROOT=Path(__file__).resolve().parent
with tempfile.TemporaryDirectory() as tmp:
    library=Path(tmp)/'encoder.so'
    subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-shared','-fPIC',str(ROOT/'tx_crypto.c'),str(ROOT/'tx_waveform.c'),'-o',str(library)],check=True)
    lib=ctypes.CDLL(str(library))
    lib.bench_packet.restype=lib.bench_waveform.restype=ctypes.c_size_t
    unique=set()
    for seq in (2,3,4294967294):
        for flood in (False,True):
            packet=(ctypes.c_uint8*64)()
            size=lib.bench_packet(packet,ctypes.c_uint32(seq),ctypes.c_uint32(1791755000),flood)
            raw=bytes(packet[:size]);cipher=raw[5:]
            assert raw[:3]==bytes([0x15 if flood else 0x16,0,0x11])
            assert raw[3:5]==hmac.digest(reference.KEY,cipher,'sha256')[:2]
            dec=Cipher(algorithms.AES(reference.KEY),modes.ECB()).decryptor()
            plain=dec.update(cipher)+dec.finalize()
            assert plain[:5]==struct.pack('<IB',1791755000,0)
            assert plain[5:].rstrip(b'\0')==f'Flipper bench: TEST {seq}'.encode()
            if not flood:
                assert cipher not in unique
                unique.add(cipher)
            for cr in (1,4):
                output=(ctypes.c_uint8*40000)();duration=ctypes.c_uint32()
                count=lib.bench_waveform(packet,size,cr,output,40000,ctypes.byref(duration))
                assert count>0 and min(output[:count])>=2
                parts=[(0,1,2048)]*32+[(8,1,2048),(16,1,2048)]+[(0,-1,2048)]*2+[(0,-1,512)]
                parts += [(s,1,2048) for s in reference.symbols(raw,cr)]
                expected=np.array(reference.waveform(parts)[0])
                actual=np.repeat(np.arange(count)%2==0,np.array(output[:count]))
                assert np.array_equal(expected,actual[:len(expected)])
                assert len(actual)==duration.value
                assert lib.bench_waveform(packet,size,cr,output,10,ctypes.byref(duration))==0
    print('PASS 12 runtime cases: independent AES/HMAC, unique messages, both routes/rates, exact pulse samples and capacity rejection')
