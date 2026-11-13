#!/usr/bin/env python3
"""Addresses for the names in doppelganger's SMB disassembly (ref/smbdis.asm, not in git).

usage: smbsyms.py ref/smbdis.asm rom/smb.nes > build/smb.sym
Walks the source assigning addresses from .org $8000 (sizes from the addressing mode, zero
page when the operand is below $100 and the opcode has such a form), then checks every
instruction's opcode against the ROM, so a wrong size anywhere is reported, not guessed.
Output: "NAME HEXVALUE" per line, labels and constants alike.
"""
import re, sys

sys.path.insert(0, __import__("os").path.dirname(__file__) + "/..")
from recomp import OPS  # opcode -> (mnemonic, mode, ...)

src = [l.split(";")[0].rstrip() for l in open(sys.argv[1], encoding="latin-1")]
rom = open(sys.argv[2], "rb").read()
prg = rom[16:16 + rom[4] * 0x4000]
FORMS = {}  # (mnemonic, mode) -> opcode
for op, v in OPS.items(): FORMS.setdefault((v[0].upper(), v[1]), op)

def num(t, sym):
    t = t.strip()
    if t.startswith("<"): return num(t[1:], sym) & 0xFF
    if t.startswith(">"): return num(t[1:], sym) >> 8
    parts = re.split(r"([+\-])", t)
    v, sign = 0, 1
    for p in parts:
        p = p.strip()
        if p == "+": sign = 1; continue
        if p == "-": sign = -1; continue
        if not p: continue
        if p.startswith("$"): x = int(p[1:], 16)
        elif p.startswith("%"): x = int(p[1:], 2)
        elif p[0].isdigit(): x = int(p)
        else: x = sym[p]  # KeyError on pass 1 for forward labels
        v += sign * x
    return v

def mode_of(mn, opnd, sym):
    """-> (mode, value or None). value None = unresolved (forward label, pass 1)."""
    o = opnd.strip()
    if not o or o.lower() == "a": return ("acc" if (mn, "acc") in FORMS else "imp"), 0
    if mn in ("BCC", "BCS", "BEQ", "BMI", "BNE", "BPL", "BVC", "BVS"): mode = "rel"; expr = o
    elif o.startswith("#"): return "imm", None
    elif o.startswith("(") and o.lower().endswith(",x)"): return "izx", None
    elif o.startswith("(") and o.lower().endswith("),y"): return "izy", None
    elif o.startswith("("): return "ind", None
    else:
        idx = ""
        if o.lower().endswith(",x"): idx, o = "x", o[:-2]
        elif o.lower().endswith(",y"): idx, o = "y", o[:-2]
        try: v = num(o, sym)
        except KeyError: v = None
        zp = v is not None and v < 0x100 and (mn, "zp" + idx) in FORMS
        mode = ("zp" if zp else "ab" if idx else "abs") + idx
        if mode == "ab": mode = "abs"
        return mode, v
    return mode, None

SIZE = {"imp": 1, "acc": 1, "imm": 2, "zp": 2, "zpx": 2, "zpy": 2, "izx": 2, "izy": 2, "rel": 2,
        "abs": 3, "abx": 3, "aby": 3, "ind": 3}

def walk(sym, check):
    pc, bad = 0, 0
    for n, line in enumerate(src, 1):
        if not line.strip(): continue
        m = re.match(r"^(\w+)\s*=\s*(.+)$", line.strip())
        if m:
            try: sym[m.group(1)] = num(m.group(2), sym)
            except KeyError: pass
            continue
        m = re.match(r"^(\w+):\s*(.*)$", line.strip())
        rest = line.strip()
        if m: sym[m.group(1)] = pc; rest = m.group(2)
        if not rest: continue
        w = rest.split(None, 1)
        d, arg = w[0].lower(), (w[1] if len(w) > 1 else "")
        if d == ".org": pc = num(arg, sym); continue
        if d in (".index", ".mem"): continue
        if d == ".db":
            items = [s for s in re.findall(r'"[^"]*"|[^,]+', arg) if s.strip()]
            pc += sum(len(s.strip()) - 2 if s.strip().startswith('"') else 1 for s in items); continue
        if d == ".dw": pc += 2 * len([s for s in arg.split(",") if s.strip()]); continue
        mn = d.upper()
        mode, _ = mode_of(mn, arg, sym)
        if check:
            got = OPS.get(prg[(pc - 0x8000) % len(prg)])
            if not got or (got[0].upper(), got[1]) != (mn, mode):
                bad += 1
                if bad <= 5: print(f"line {n}: {rest!r} at {pc:04X}: ROM has {got}", file=sys.stderr)
        pc += SIZE[mode]
    return bad

sym = {}
walk(sym, False)  # pass 1: learn label addresses
walk(sym, False)  # pass 2: forward labels now known, sizes settle
bad = walk(sym, True)
for k, v in sorted(sym.items(), key=lambda kv: kv[1]): print(f"{k} {v:04X}")
print(f"smbsyms: {len(sym)} names, {bad} instructions disagree with the ROM", file=sys.stderr)
sys.exit(1 if bad else 0)
