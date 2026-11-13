#!/usr/bin/env python3
"""Static recompiler: iNES ROM (mapper 0, 2, 3 or 7) -> one C file of code (no ROM data: the
runtime loads the ROM file itself and checks it is the one this code was generated from).

usage: recomp.py ROM OUT.c [MISSES.txt] [--entry HEX ...] [--mods MODS.txt] [--lst OUT.lst] [--syms SYMS.txt]

Each 32K view of the PRG ROM becomes one C function (mapper 2 switches only the lower 16K,
so its views are "16K bank i + the fixed last 16K", one per i): a switch on pc with a case per
discovered instruction. The same opcode templates also generate interp_step(),
the fallback for code the descent did not find (indirect-jump targets, code the
game copies into RAM). The runtime logs those misses; feeding MISSES.txt back in
compiles them. RAM code becomes run_ram(): one case per (address, opcode) seen
there, operands read from RAM at run time, so self-modified operands still work
and a changed opcode falls back to the interpreter.
"""
import hashlib, sys

TABLE = """
00 BRK imp 7; 01 ORA izx 6; 05 ORA zp 3; 06 ASL zp 5; 08 PHP imp 3; 09 ORA imm 2; 0A ASL acc 2; 0D ORA abs 4; 0E ASL abs 6;
10 BPL rel 2; 11 ORA izy 5+; 15 ORA zpx 4; 16 ASL zpx 6; 18 CLC imp 2; 19 ORA aby 4+; 1D ORA abx 4+; 1E ASL abx 7;
20 JSR abs 6; 21 AND izx 6; 24 BIT zp 3; 25 AND zp 3; 26 ROL zp 5; 28 PLP imp 4; 29 AND imm 2; 2A ROL acc 2; 2C BIT abs 4; 2D AND abs 4; 2E ROL abs 6;
30 BMI rel 2; 31 AND izy 5+; 35 AND zpx 4; 36 ROL zpx 6; 38 SEC imp 2; 39 AND aby 4+; 3D AND abx 4+; 3E ROL abx 7;
40 RTI imp 6; 41 EOR izx 6; 45 EOR zp 3; 46 LSR zp 5; 48 PHA imp 3; 49 EOR imm 2; 4A LSR acc 2; 4C JMP abs 3; 4D EOR abs 4; 4E LSR abs 6;
50 BVC rel 2; 51 EOR izy 5+; 55 EOR zpx 4; 56 LSR zpx 6; 58 CLI imp 2; 59 EOR aby 4+; 5D EOR abx 4+; 5E LSR abx 7;
60 RTS imp 6; 61 ADC izx 6; 65 ADC zp 3; 66 ROR zp 5; 68 PLA imp 4; 69 ADC imm 2; 6A ROR acc 2; 6C JMP ind 5; 6D ADC abs 4; 6E ROR abs 6;
70 BVS rel 2; 71 ADC izy 5+; 75 ADC zpx 4; 76 ROR zpx 6; 78 SEI imp 2; 79 ADC aby 4+; 7D ADC abx 4+; 7E ROR abx 7;
81 STA izx 6; 84 STY zp 3; 85 STA zp 3; 86 STX zp 3; 88 DEY imp 2; 8A TXA imp 2; 8C STY abs 4; 8D STA abs 4; 8E STX abs 4;
90 BCC rel 2; 91 STA izy 6; 94 STY zpx 4; 95 STA zpx 4; 96 STX zpy 4; 98 TYA imp 2; 99 STA aby 5; 9A TXS imp 2; 9D STA abx 5;
A0 LDY imm 2; A1 LDA izx 6; A2 LDX imm 2; A4 LDY zp 3; A5 LDA zp 3; A6 LDX zp 3; A8 TAY imp 2; A9 LDA imm 2; AA TAX imp 2; AC LDY abs 4; AD LDA abs 4; AE LDX abs 4;
B0 BCS rel 2; B1 LDA izy 5+; B4 LDY zpx 4; B5 LDA zpx 4; B6 LDX zpy 4; B8 CLV imp 2; B9 LDA aby 4+; BA TSX imp 2; BC LDY abx 4+; BD LDA abx 4+; BE LDX aby 4+;
C0 CPY imm 2; C1 CMP izx 6; C4 CPY zp 3; C5 CMP zp 3; C6 DEC zp 5; C8 INY imp 2; C9 CMP imm 2; CA DEX imp 2; CC CPY abs 4; CD CMP abs 4; CE DEC abs 6;
D0 BNE rel 2; D1 CMP izy 5+; D5 CMP zpx 4; D6 DEC zpx 6; D8 CLD imp 2; D9 CMP aby 4+; DD CMP abx 4+; DE DEC abx 7;
E0 CPX imm 2; E1 SBC izx 6; E4 CPX zp 3; E5 SBC zp 3; E6 INC zp 5; E8 INX imp 2; E9 SBC imm 2; EA NOP imp 2; EC CPX abs 4; ED SBC abs 4; EE INC abs 6;
F0 BEQ rel 2; F1 SBC izy 5+; F5 SBC zpx 4; F6 INC zpx 6; F8 SED imp 2; F9 SBC aby 4+; FD SBC abx 4+; FE INC abx 7
"""
OPS = {}
for ent in TABLE.replace("\n", " ").split(";"):
    code, mn, mode, cy = ent.split()
    OPS[int(code, 16)] = (mn, mode, int(cy.rstrip("+")), cy.endswith("+"))
assert len(OPS) == 151, len(OPS)  # the official 6502 opcode count
OFFICIAL = set(OPS)
# Unofficial opcodes (the stable ones, all that nestest exercises): multi-byte NOPs, LAX, SAX,
# SBC $EB, and the read-modify-write combos DCP ISB SLO RLA SRE RRA.
def _un(codes, mn, mode, cy, pen=False):
    for c in codes.split(): OPS[int(c, 16)] = (mn, mode, cy, pen)
_un("1A 3A 5A 7A DA FA", "NOP", "imp", 2); _un("80 82 89 C2 E2", "NOP", "imm", 2); _un("04 44 64", "NOP", "zp", 3)
_un("14 34 54 74 D4 F4", "NOP", "zpx", 4); _un("0C", "NOP", "abs", 4); _un("1C 3C 5C 7C DC FC", "NOP", "abx", 4, True)
_un("A3", "LAX", "izx", 6); _un("A7", "LAX", "zp", 3); _un("AF", "LAX", "abs", 4)
_un("B3", "LAX", "izy", 5, True); _un("B7", "LAX", "zpy", 4); _un("BF", "LAX", "aby", 4, True)
_un("83", "SAX", "izx", 6); _un("87", "SAX", "zp", 3); _un("8F", "SAX", "abs", 4); _un("97", "SAX", "zpy", 4)
_un("EB", "SBC", "imm", 2)
for _h, _mn in ((0xC0, "DCP"), (0xE0, "ISB"), (0x00, "SLO"), (0x20, "RLA"), (0x40, "SRE"), (0x60, "RRA")):
    for _lo, _mode, _cy in ((0x03, "izx", 8), (0x07, "zp", 5), (0x0F, "abs", 6), (0x13, "izy", 8),
                            (0x17, "zpx", 6), (0x1B, "aby", 7), (0x1F, "abx", 7)):
        OPS[_h + _lo] = (_mn, _mode, _cy, False)
assert len(OPS) == 151 + 80, len(OPS)

LEN = dict(imp=1, acc=1, imm=2, zp=2, zpx=2, zpy=2, izx=2, izy=2, rel=2, abs=3, abx=3, aby=3, ind=3)
READ = dict(ORA="A|=m;NZ(A);", AND="A&=m;NZ(A);", EOR="A^=m;NZ(A);", ADC="ADC(m);", SBC="ADC(m^0xff);",
            CMP="CMP(A,m);", CPX="CMP(X,m);", CPY="CMP(Y,m);", BIT="Z=!(A&m);N=m>>7;V=(m>>6)&1;",
            LDA="A=m;NZ(A);", LDX="X=m;NZ(X);", LDY="Y=m;NZ(Y);", LAX="A=X=m;NZ(A);", NOP="(void)m;")
RMW = dict(ASL="C=m>>7;m<<=1;", LSR="C=m&1;m>>=1;", ROL="{u8 c=m>>7;m=(m<<1)|C;C=c;}",
           ROR="{u8 c=m&1;m=(m>>1)|(C<<7);C=c;}", INC="m++;", DEC="m--;")
COMBO = dict(DCP=("DEC", "CMP(A,m);"), ISB=("INC", "ADC(m^0xff);"), SLO=("ASL", "A|=m;NZ(A);"),
             RLA=("ROL", "A&=m;NZ(A);"), SRE=("LSR", "A^=m;NZ(A);"), RRA=("ROR", "ADC(m);"))  # RMW op, then this
STORE = dict(STA="A", STX="X", STY="Y", SAX="A&X")
IMP = dict(CLC="C=0;", SEC="C=1;", CLI="I=0;IRQCHK;", SEI="I=1;", CLV="V=0;", CLD="D=0;", SED="D=1;", NOP="",
           TAX="X=A;NZ(X);", TAY="Y=A;NZ(Y);", TXA="A=X;NZ(A);", TYA="A=Y;NZ(A);", TSX="X=S;NZ(X);", TXS="S=X;",
           INX="X++;NZ(X);", INY="Y++;NZ(Y);", DEX="X--;NZ(X);", DEY="Y--;NZ(Y);",
           PHA="PUSH(A);", PHP="PUSH(getp()|0x10);", PLA="A=PULL();NZ(A);", PLP="setp(PULL());")
BR = dict(BPL="!N", BMI="N", BVC="!V", BVS="V", BCC="!C", BCS="C", BNE="!Z", BEQ="Z")
ENDS = ("JMP", "RTS", "RTI", "BRK")


def ea(mode, B, W, pen):
    if mode == "zp": return f"u16 ea={B};"
    if mode in ("zpx", "zpy"): return f"u16 ea=(u8)({B}+{mode[2].upper()});"
    if mode == "abs": return f"u16 ea={W};"
    if mode in ("abx", "aby"):
        r = mode[2].upper()
        return f"u16 ea={W}+{r};" + (f"cyc+=((({W})&0xff)+{r})>>8;" if pen else "")
    if mode == "izx": return f"u16 ea=rd16zp((u8)({B}+X));"
    if mode == "izy": return f"u16 b_=rd16zp({B});u16 ea=b_+Y;" + ("cyc+=((b_&0xff)+Y)>>8;" if pen else "")
    raise ValueError(mode)


def emit(op, PC, B, W, jump):
    """C for one instruction. PC/B/W are C expressions (literals when recompiling,
    runtime variables in the interpreter). jump(expr) emits a control transfer.
    Returns (code, is_unconditional_flow, next_pc_expr)."""
    mn, mode, cy, pen = OPS[op]
    NEXT = f"(u16)({PC}+{LEN[mode]})"
    s, flow = f"cyc+={cy};", mn in ENDS or mn == "JSR"
    if mn in READ and mode != "imp":
        s += (f"u8 m={B};" if mode == "imm" else ea(mode, B, W, pen) + "u8 m=rd(ea);") + READ[mn]
    elif mn in RMW or mn in COMBO:
        base, post = COMBO.get(mn, (mn, "NZ(m);"))
        if mode == "acc": s += RMW[mn].replace("m", "A") + "NZ(A);"
        else: s += ea(mode, B, W, False) + "u8 m=rd(ea);" + RMW[base] + "wr(ea,m);" + post
    elif mn in STORE: s += ea(mode, B, W, False) + f"wr(ea,{STORE[mn]});"
    elif mn in IMP: s += IMP[mn]
    elif mn in BR:
        T = f"(u16)({PC}+2+(int8_t)({B}))"
        s += f"if({BR[mn]}){{cyc+=1+(((({T})^({NEXT}))>>8)!=0);{jump(T)}}}"
    elif mn == "JMP":
        s += jump(W) if mode == "abs" else f"pc=rd({W})|rd((({W})&0xff00)|(u8)({W}+1))<<8;break;"
    elif mn == "JSR": s += f"{{u16 r={PC}+2;PUSH(r>>8);PUSH(r);}}" + jump(W)
    elif mn == "RTS": s += "{u8 l=PULL();pc=(l|PULL()<<8)+1;}break;"
    elif mn == "RTI": s += "setp(PULL());{u8 l=PULL();pc=l|PULL()<<8;}break;"
    elif mn == "BRK": s += f"{{u16 r={PC}+2;PUSH(r>>8);PUSH(r);PUSH(getp()|0x10);I=1;pc=rd16(0xFFFE);}}break;"
    return s, flow, NEXT


def decode(img, a, ops=OPS):
    """(op, len, byte operand, word operand, static target or None) at address a, or None."""
    op = img[a - 0x8000]
    if op not in ops: return None
    mn, mode = OPS[op][:2]
    n = LEN[mode]
    if a + n > 0x10000: return None
    b = img[a + 1 - 0x8000] if n > 1 else 0
    w = b | (img[a + 2 - 0x8000] << 8) if n > 2 else 0
    tgt = None
    if mn in BR: tgt = (a + 2 + (b - 256 if b > 127 else b)) & 0xFFFF
    elif mn in ("JMP", "JSR") and mode == "abs": tgt = w
    return op, n, b, w, tgt


def descend(img, entries):
    """Recursive descent. Returns ({addr: decoded}, {addresses following a write to $8000+})."""
    seen, after_bankswitch, work = {}, set(), list(entries)
    while work:
        a = work.pop()
        while 0x8000 <= a <= 0xFFFF and a not in seen:
            d = decode(img, a)
            if not d: break
            seen[a] = d
            op, n, b, w, tgt = d
            mn, mode = OPS[op][:2]
            if tgt is not None: work.append(tgt)
            if mn in STORE and mode in ("abs", "abx", "aby") and w >= 0x8000: after_bankswitch.add(a + n)
            if mn in ENDS: break
            a += n
    return seen, after_bankswitch


def looks_like_code(img, a):
    """True if a straight-line decode from a reaches RTS/RTI/JMP within 48 instructions
    without meeting an invalid opcode or BRK."""
    for _ in range(48):
        if not 0x8000 <= a <= 0xFFFF: return False
        d = decode(img, a, OFFICIAL)  # guessing: unofficial opcodes count as "not code"
        if not d: return False
        mn = OPS[d[0]][0]
        if mn == "BRK": return False
        if mn in ENDS: return True
        a += d[1]
    return False


def scan_pointers(img):
    """Jump tables are invisible to the descent, so guess: any 16-bit word in the bank that
    points at something code-shaped becomes an entry point (and word+1, for RTS-style tables
    that store target-1). A wrong guess only costs dead code: every case is still a faithful
    translation of the bytes at its own address."""
    out = set()
    for i in range(len(img) - 1):
        w = img[i] | img[i + 1] << 8
        for t in (w, w + 1):
            if 0x8000 <= t <= 0xFFF9 and looks_like_code(img, t): out.add(t)
    return out


FMT = dict(imp="", acc="A", imm="#${b:02X}", zp="{n}", zpx="{n},X", zpy="{n},Y", izx="({n},X)", izy="({n}),Y",
           abs="{n}", abx="{n},X", aby="{n},Y", ind="({n})", rel="{n}")
HW = {0x2000: "PPUCTRL", 0x2001: "PPUMASK", 0x2002: "PPUSTATUS", 0x2003: "OAMADDR", 0x2004: "OAMDATA", 0x2005: "PPUSCROLL",
      0x2006: "PPUADDR", 0x2007: "PPUDATA", 0x4010: "DMC_FREQ", 0x4011: "DMC_RAW", 0x4012: "DMC_START", 0x4013: "DMC_LEN",
      0x4014: "OAMDMA", 0x4015: "SND_CHN", 0x4016: "JOY1", 0x4017: "JOY2"}


def listing(path, banks, res, mods, symfile):
    """Readable disassembly of everything the descent found: subroutines split out and labelled
    (sub_ = JSR target, loc_ = branch/JMP target), call counts, names from SYMS.txt, mods marked.
    SYMS.txt lines: 'ADDR name' for memory, 'BANK:ADDR name' for code; '#' starts a comment."""
    data, code = dict(HW), {}
    if symfile:
        for line in open(symfile):
            f = line.split("#")[0].split()
            if len(f) == 2 and ":" in f[0]: code[int(f[0].split(":")[0]), int(f[0].split(":")[1], 16)] = f[1]
            elif len(f) == 2: data[int(f[0], 16)] = f[1]
    with open(path, "w") as out:
        for bi, (seen, _) in enumerate(res):
            calls, jumps = {}, set()
            for op, n, b, w, tgt in seen.values():
                if tgt is None: continue
                if OPS[op][0] == "JSR": calls[tgt] = calls.get(tgt, 0) + 1
                else: jumps.add(tgt)
            def label(t):
                if (bi, t) in code: return code[bi, t]
                return f"sub_{t:04X}" if t in calls else f"loc_{t:04X}" if t in jumps else None
            out.write(f"\n; ======== bank {bi}: {len(seen)} instructions, {len(calls)} subroutines ========\n")
            prev_end = True
            for a in sorted(seen):
                op, n, b, w, tgt = seen[a]
                mn, mode = OPS[op][:2]
                if label(a):
                    out.write(("\n" if a in calls or prev_end else "") + f"{label(a)}:" +
                              (f"    ; called from {calls[a]} place{'s' * (calls[a] > 1)}" if a in calls else "") + "\n")
                elif prev_end: out.write("\n")
                if (bi, a) in mods: out.write(f"      ; MOD (mods.txt): {mods[bi, a][1]}\n")
                t = tgt if tgt is not None else w if LEN[mode] == 3 else b
                name = (label(t) if tgt is not None else data.get(t)) or (f"${t:04X}" if LEN[mode] == 3 or tgt is not None else f"${t:02X}")
                raw = " ".join(f"{x:02X}" for x in banks[bi][a - 0x8000:a - 0x8000 + n])
                out.write(f"  {a:04X}  {raw:<9} {mn} {FMT[mode].format(b=b, n=name)}".rstrip() + "\n")
                prev_end = mn in ENDS


def main():
    args = sys.argv[1:]
    extra = set()
    while "--entry" in args:
        i = args.index("--entry"); extra.add(int(args[i + 1], 16)); del args[i:i + 2]
    mods = {}  # (bank, addr) -> [expected instruction bytes or None, C code run before that instruction]; see mods.txt
    mods_sha1 = None
    if "--mods" in args:
        i = args.index("--mods")
        for line in open(args[i + 1]):
            line = line.split("#")[0].strip()
            if line.startswith("rom-sha1"): mods_sha1 = line.split()[1].lower()
            elif line:
                bank, addr, code = line.split(None, 2)
                want = None
                if code.startswith("["):  # [E6 0D] = the bytes this mod expects to find there
                    want, code = code[1:].split("]", 1)
                    want = bytes.fromhex(want)
                mods[int(bank), int(addr, 16)] = (want, code.strip())
        del args[i:i + 2]
    opt = {}
    for flag in ("--lst", "--syms"):
        if flag in args:
            i = args.index(flag); opt[flag] = args[i + 1]; del args[i:i + 2]
    rom = open(args[0], "rb").read()
    # Mods are addresses in one exact ROM; on any other dump they would patch the wrong code.
    assert not mods or not mods_sha1 or hashlib.sha1(rom).hexdigest() == mods_sha1, \
        f"{args[0]} is not the ROM the mods were written for (sha1 {hashlib.sha1(rom).hexdigest()}, want {mods_sha1})"
    assert rom[:4] == b"NES\x1a", "not an iNES file"
    mapper, mirror_v = (rom[6] >> 4) | (rom[7] & 0xF0), rom[6] & 1
    assert mapper in (0, 2, 3, 7), f"mapper {mapper} not supported (only 0, 2, 3 and 7)"
    off = 16 + (512 if rom[6] & 4 else 0)
    prg = rom[off:off + rom[4] * 0x4000]
    chr_ = rom[off + len(prg):off + len(prg) + rom[5] * 0x2000]
    if len(prg) == 0x4000: prg *= 2  # NROM-128: 16K mirrored
    if mapper == 2: banks = [prg[i:i + 0x4000] + prg[-0x4000:] for i in range(0, len(prg), 0x4000)]  # rt.c load_rom builds the same views
    else: banks = [prg[i:i + 0x8000] for i in range(0, len(prg), 0x8000)]

    entries = [set(extra) | {img[i] | img[i + 1] << 8 for i in (0x7FFA, 0x7FFC, 0x7FFE)} for img in banks]
    for i, img in enumerate(banks): entries[i] |= scan_pointers(img)
    for bank, addr in mods: entries[bank].add(addr)
    ramcode = {}  # RAM address -> opcodes seen executing there ("R ADDR OP" lines)
    if len(args) > 2:
        try:
            for line in open(args[2]):
                f = line.split()
                if f[0] == "R": ramcode.setdefault(int(f[1], 16), set()).add(int(f[2], 16))
                else: entries[int(f[0])].add(int(f[1], 16))
        except FileNotFoundError: pass
    # After a write to $8000+ the next instruction is fetched from whichever bank got
    # mapped in, so that address is an entry point in every bank. Iterate to fixpoint.
    shared = set()
    while True:
        res = [descend(img, entries[i] | shared) for i, img in enumerate(banks)]
        new = set().union(*(r[1] for r in res)) - shared
        if not new: break
        shared |= new

    fnv = 0xcbf29ce484222325  # FNV-1a 64 of the ROM file; the runtime refuses any other file
    for byte in rom: fnv = ((fnv ^ byte) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    if "--lst" in opt: listing(opt["--lst"], banks, res, mods, opt.get("--syms"))
    o = [f"/* generated by recomp.py from {args[0]} - do not edit */", '#include "rt.h"',
         f"const int NBANKS={len(banks)},MAPPER={mapper},MIRROR_V={mirror_v},CHR_SIZE={len(chr_)};",
         f"const int ROM_SIZE={len(rom)},PRG_OFF={off},PRG_LEN={rom[4] * 0x4000};const u64 ROM_HASH=0x{fnv:016x}ull;"]

    o.append("int interp_step(void){u8 op=rd(pc),ob=rd(pc+1);u16 ow=ob|rd(pc+2)<<8;TR(pc);switch(op){")
    for op in sorted(OPS):
        s, flow, nxt = emit(op, "pc", "ob", "ow", lambda T: f"pc={T};break;")
        o.append(f"case {op}:{{{s}{'' if flow else f'pc={nxt};'}}}break;")
    o.append("default:die(op);}return 0;}")

    for bi, (seen, _) in enumerate(res):
        def jump(_expr, seen=seen):
            if tgt in seen: return f"pc={tgt};if(cyc>=cyc_target)return 0;goto L{tgt:04X};"
            return f"pc={tgt};break;"
        o.append(f"static int run_bank{bi}(void){{for(;;){{if(cyc>=cyc_target)return 0;switch(pc){{")
        order = sorted(seen)
        for i, a in enumerate(order):
            op, n, b, w, tgt = seen[a]
            s, flow, _ = emit(op, str(a), str(b), str(w), jump)
            if (bi, a) in mods:
                assert not flow and OPS[op][0] not in BR, f"mod at {bi}:{a:04X}: jumps and branches cannot be modded"
                assert seen[a] == decode(banks[bi], a)
                want, code = mods[bi, a]
                have = banks[bi][a - 0x8000:a - 0x8000 + n]
                assert want is None or want == have, f"mod at {bi}:{a:04X}: expected bytes {want.hex()}, ROM has {have.hex()}"
                # SKIP charges the instruction's cycles and jumps past its body
                s = f"\n#define SKIP {{cyc+={OPS[op][2]};goto S{a:04X};}}\n{code}\n#undef SKIP\n{s}S{a:04X}:;"
            if not flow:
                s += f"if(cyc>=cyc_target){{pc={(a + n) & 0xFFFF};return 0;}}"
                if a + n not in seen: s += f"pc={(a + n) & 0xFFFF};break;"
                # overlapping instructions: the next case in the file is not the next instruction
                elif order[i + 1] != a + n: s += f"goto L{a + n:04X};"
            o.append(f"case {a}:L{a:04X}:{{TR({a});{s}}}")
        o.append("default:return pc>=0x8000;}}}")  # 0 = jumped into RAM: let the caller dispatch
        print(f"bank {bi}: {len(seen)} instructions, {sum(1 for k in mods if k[0] == bi)} mods", file=sys.stderr)
    o.append("int run_ram(void){for(;;){if(cyc>=cyc_target)return 0;switch(pc){")
    for a in sorted(ramcode):
        o.append(f"case {a}:{{TR({a});switch(ram[{a}]){{")
        for op in sorted(ramcode[a] & set(OPS)):
            B, W = f"ram[{(a + 1) & 0x7ff}]", f"(ram[{(a + 1) & 0x7ff}]|ram[{(a + 2) & 0x7ff}]<<8)"
            s, flow, nxt = emit(op, str(a), B, W, lambda T: f"pc={T};break;")
            o.append(f"case {op}:{{{s}{'' if flow else f'pc={nxt};'}}}break;")
        o.append("default:return 1;}continue;}")
    o.append("default:return pc<0x800;}}}")  # 0 = left RAM: let the caller dispatch to the ROM code
    print(f"ram: {len(ramcode)} instruction addresses", file=sys.stderr)
    o.append("int(*const bank_fn[])(void)={" + ",".join(f"run_bank{i}" for i in range(len(banks))) + "};")
    open(args[1], "w").write("\n".join(o) + "\n")


if __name__ == "__main__":
    main()
