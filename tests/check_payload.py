"""End-to-end public synthetic packets; private capture regression when present."""
from pathlib import Path
import subprocess
import tempfile
import sys

ROOT = Path(__file__).resolve().parent.parent
HOST = ROOT / 'tests/host_payload'


def crc16(data):
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ (0x1021 if crc & 0x8000 else 0)) & 65535
    return crc


def encode_nibble(n, cr):
    d = [(n >> i) & 1 for i in range(4)]
    if cr == 1:
        bits = d + [d[0] ^ d[1] ^ d[2] ^ d[3]]
    else:
        bits = d + [d[0]^d[1]^d[2], d[1]^d[2]^d[3],
                    d[0]^d[1]^d[3], d[0]^d[2]^d[3]]
        bits = bits[:4+cr]
    return bits


def block(nibbles, sf, cr):
    cws = [encode_nibble(n, cr) for n in nibbles]
    result = []
    for i in range(4+cr):
        gray = 0
        for j in range(sf):
            gray = (gray << 1) | cws[(i-j-1) % sf][i]
        value, shifted = gray, gray >> 1
        while shifted:
            value ^= shifted
            shifted >>= 1
        result.append((value * (4 if sf == 5 else 1) + 1) % 128)
    return result


def packet(cr=1, damage=False):
    data = bytes([0x11, 0]) + bytes(range(32)) + bytes([1,2,3,4]) + bytes(64) + bytes([0x82]) + b'TestNode'
    h = [len(data) >> 4, len(data) & 15, cr * 2 + 1]
    masks = [(15,0,0), (8,14,1), (4,9,10), (2,5,7), (1,2,15)]
    check = 0
    for masks_row in masks:
        parity = sum(bin(a & b).count('1') for a,b in zip(h, masks_row)) % 2
        check = check * 2 + parity
    syms = block(h + [check >> 4, check & 15], 5, 4)
    whitening, encoded = 255, []
    for byte in data:
        encoded.append(byte ^ whitening)
        fb = ((whitening >> 7) ^ (whitening >> 5) ^ (whitening >> 4) ^ (whitening >> 3)) & 1
        whitening = ((whitening << 1) | fb) & 255
    crc = crc16(data[:-2]) ^ data[-1] ^ (data[-2] << 8)
    encoded += [crc & 255, crc >> 8]
    nibbles = [n for byte in encoded for n in (byte & 15, byte >> 4)]
    for i in range(0, len(nibbles), 7):
        row = nibbles[i:i+7]
        syms += block(row + [0] * (7-len(row)), 7, cr)
    if damage:
        syms[12:] = [0] * (len(syms)-12)
    # 1 us sampled chirps, matching the CC1101 discriminator sign convention.
    wave = [True] * 1024
    def chirp(shift):
        wave.extend(((i + shift*16) % 2048) >= 1300 for i in range(2048))
    for _ in range(31):
        chirp(0)
    chirp(8)
    chirp(16)
    # Downchirp delimiter, then exactly 1/4 symbol. The payload decoder ignores it.
    wave.extend([True]*2048 + [False]*2048 + [True]*512)
    for sym in syms:
        chirp(sym)
    wave.extend([True]*8192)
    durations = []
    last, run = wave[0], 0
    for level in wave:
        if level != last:
            durations.append(run if last else -run)
            run, last = 0, level
        run += 1
    durations.append(run if last else -run)
    return 'Filetype: Flipper SubGhz RAW File\nRAW_Data: ' + ' '.join(map(str,durations)) + '\n'


with tempfile.TemporaryDirectory() as tmp:
    for cr in range(1,5):
        path = Path(tmp) / f'clean-{cr}.sub'
        path.write_text(packet(cr))
        output = subprocess.check_output([str(HOST), str(path)], text=True)
        assert 'status=2 length=111' in output and 'type=4 name=TestNode' in output, output
        print(f'PASS synthetic SF7 advert, CR4/{4+cr}, CRC and node name')
    path = Path(tmp) / 'damaged.sub'
    path.write_text(packet(damage=True))
    output = subprocess.check_output([str(HOST), str(path)], text=True)
    assert 'status=1' in output and 'status=2' not in output, output
    print('PASS damaged packet is not CRC-checked')

capture = ROOT / 'captures/RAW-20261010-140003.sub'
if capture.exists():
    output = subprocess.check_output([str(HOST), str(capture)], text=True)
    assert output.count('payload t=') == 3, output
    reference = subprocess.check_output([sys.executable, str(ROOT / 'tools/meshcore_advert.py'), str(capture)], text=True)
    assert 'signature VALID' in reference
    name = next(line.split('name', 1)[1].strip() for line in reference.splitlines() if line.strip().startswith('name '))
    assert f'type=4 name={name}' in output, 'C candidate differs from offline verified name'
    assert 'status=2' not in output, 'Private damaged capture must remain unverified'
    print('PASS private MeshCore advert combining: node name recovered, still unverified')
else:
    print('SKIP private MeshCore advert capture')
