"""usage: python3 -I dis.py ROM.nes BANK HEXADDR [COUNT]
Disassemble raw PRG bytes at any address (uses recomp.py's opcode table). For the code the
recompiler found, the labelled listing build/NAME.lst is the better read."""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I drops the script dir
import recomp

rom = open(sys.argv[1], "rb").read()[16:]
bank, addr = int(sys.argv[2]), int(sys.argv[3], 16)
img = rom[bank * 0x8000:(bank + 1) * 0x8000]
for _ in range(int(sys.argv[4]) if len(sys.argv) > 4 else 40):
    d = recomp.decode(img, addr)
    if not d:
        print("%04X  .db $%02X" % (addr, img[addr - 0x8000])); addr += 1; continue
    op, n, b, w, tgt = d
    mn, mode = recomp.OPS[op][:2]
    t = tgt if tgt is not None else w if n == 3 else b
    raw = " ".join("%02X" % x for x in img[addr - 0x8000:addr - 0x8000 + n])
    print("%04X  %-9s %s %s" % (addr, raw, mn, recomp.FMT[mode].format(b=b, n=("$%04X" if n == 3 or tgt is not None else "$%02X") % t)))
    addr += n
