"""Independent Ed25519/X25519 generation checks; no real device keys involved."""
import ctypes as C
import hashlib
import subprocess
import tempfile
from pathlib import Path
from cryptography.hazmat.primitives.asymmetric import ed25519, x25519
from cryptography.hazmat.primitives.serialization import Encoding, PublicFormat
ROOT=Path(__file__).resolve().parent.parent
class Identity(C.Structure):
    _fields_=[('uid',C.c_uint8*12),('node',C.c_uint32),('mc_private',C.c_uint8*64),
              ('mc_public',C.c_uint8*32),('mt_private',C.c_uint8*32),('mt_public',C.c_uint8*32)]
with tempfile.TemporaryDirectory() as tmp:
    tmp=Path(tmp)
    (tmp/'rng.c').write_text('void randombytes(unsigned char* p, unsigned long long n) { static unsigned x; while(n--) *p++ = x++; }')
    so=tmp/'identity.so'
    subprocess.run(['cc','-shared','-fPIC','-O2',str(ROOT/'app/identity.c'),str(ROOT/'app/identity_name.c'),
                    str(ROOT/'app/crypto/tweetnacl.c'),str(tmp/'rng.c'),'-o',str(so)],check=True)
    lib=C.CDLL(str(so))
    identity=Identity();lib.lora_identity_generate(C.byref(identity))
    seed=bytes(range(32))
    expected=ed25519.Ed25519PrivateKey.from_private_bytes(seed).public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
    assert bytes(identity.mc_public)==expected
    expanded=bytearray(hashlib.sha512(seed).digest());expanded[0]&=248;expanded[31]&=63;expanded[31]|=64
    assert bytes(identity.mc_private)==expanded
    mt=x25519.X25519PrivateKey.from_private_bytes(bytes(identity.mt_private))
    assert bytes(identity.mt_public)==mt.public_key().public_bytes(Encoding.Raw,PublicFormat.Raw)
    assert bytes(identity.mc_public)!=bytes(identity.mt_public)
    another=Identity();lib.lora_identity_generate(C.byref(another))
    assert bytes(identity.mc_public)!=bytes(another.mc_public)
    assert bytes(identity.mt_public)!=bytes(another.mt_public)
    for good in (b'Flipper',b'A',b'ABCDEFGHIJKLMNOP',b'My Flipper'):
        assert lib.lora_name_valid(good)
        assert lib.lora_mc_message_limit(good)==41-len(good)
    for bad in (b'',b' bad',b'bad ',b'bad:name',b'bad\nname',b'X'*17):
        assert not lib.lora_name_valid(bad)
        assert not lib.lora_mc_message_limit(bad)
print('PASS independent Ed25519 expanded-secret and X25519 vectors, distinct identities, and name bounds')
