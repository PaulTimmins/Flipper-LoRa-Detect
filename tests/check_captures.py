"""Private captures are optional locally, never copied into test fixtures."""
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parent.parent
for name, bw, count, sf, sync, preamble in [
    ('RAW-20261010-140003.sub', 62500, 3, 7, '12', 32),
    ('RAW-20261010-142535.sub', 250000, 2, 11, '2B', 16),
]:
    path = root / 'captures' / name
    if not path.exists():
        print(f'SKIP private capture {name}')
        continue
    result = subprocess.run([str(root / 'tests/host_detect'), str(path), str(bw)],
                            check=True, text=True, capture_output=True)
    lines = result.stdout.splitlines()
    assert len(lines) == count, result.stdout
    for line in lines:
        assert f'SF{sf} BW={bw} preamble={preamble} sync={sync} ' in line, line
    print(f'PASS {name}: {count} packets SF{sf}, sync {sync}, preamble {preamble}')
