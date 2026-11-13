#!/usr/bin/env python3
"""Random-play coverage: runs build/smb with random input (mostly running right and jumping,
so levels get played), which appends interpreter misses to misses-smb.txt. Then `make`.
usage: python3 -I tools/cover.py [RUNS] [FRAMES]"""
import random, subprocess, sys

runs = int(sys.argv[1]) if len(sys.argv) > 1 else 12
frames = int(sys.argv[2]) if len(sys.argv) > 2 else 8000
for seed in range(runs):
    r = random.Random(seed)
    keys = ["200:08", "210:00"]  # Start on the title
    f = 300
    while f < frames:
        # bits: A B Select Start Up Down Left Right
        b = r.choice([0x81, 0x83, 0x80, 0x82, 0x01, 0x40, 0x41, 0x20, 0x10, 0x00, 0x08, 0x81, 0x83])
        keys.append(f"{f}:{b:02X}")
        f += r.randint(4, 60)
    out = subprocess.run(["build/smb", "--frames", str(frames), "--keys", ",".join(keys)],
                         capture_output=True, text=True).stderr.strip().splitlines()
    print(f"seed {seed}: {out[-1] if out else '?'}")
