#!/usr/bin/env python3
"""Compare our CPU trace (recompiled and interpreted) against testroms/nestest.log.

PASS = our trace matches every line of the reference (pc, A, X, Y, P, SP, cycle count),
including its unofficial-opcode section ('*' lines).
"""
import re, subprocess, sys

ref = []
for line in open("testroms/nestest.log"):
    m = re.match(r"([0-9A-F]{4}) .*?A:(..) X:(..) Y:(..) P:(..) SP:(..).*CYC:(\d+)", line)
    ref.append((m.groups(), "*" in line[:20]))
n_unofficial = sum(u for _, u in ref)

ok = True
for mode in ([], ["--interp"]):
    out = subprocess.run(["build/nestest", "--trace", "--pc", "C000", "--frames", "1", *mode],
                         capture_output=True, text=True).stdout.splitlines()
    got = [re.match(r"(....) A:(..) X:(..) Y:(..) P:(..) SP:(..) CYC:(\d+)", l).groups() for l in out]
    # the reference ends when nestest returns to its caller; whatever we run after that is not compared
    bad = next((i for i, r in enumerate(ref) if i >= len(got) or got[i] != r[0]), None)
    name = "interp" if mode else "recomp"
    if bad is not None:
        ok = False
        print(f"FAIL {name}: line {bad + 1}: got {got[bad] if bad < len(got) else None} want {ref[bad][0]}")
    else:
        print(f"PASS {name}: all {len(ref)} reference instructions match ({n_unofficial} of them unofficial opcodes)")
sys.exit(0 if ok else 1)
