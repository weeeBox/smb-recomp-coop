// Runtime for the recompiled game: memory map, scanline-level PPU renderer, input, SDL shell.
// The game's CPU code is in the generated file, compiled separately; rt.h is the interface.
// The game's data (graphics, levels, sound) is NOT compiled in: load_rom() reads the user's
// own ROM file at startup and refuses any file but the one the code was generated from.
//
// ponytail: each line is drawn from the PPU state at its first dot, so a register write
//           mid-line shows from the next line; per-dot effects need a dot-level renderer.
//           Flags, sprite-0 hit, scroll stepping and NMI ARE timed to the dot.
#include <stdlib.h>
#include <string.h>
#ifndef RT_LIB  // android/main.c includes this file as a library, without SDL or main()
#include <SDL.h>
#endif
#include "rt.h"
extern u8 luigi_oam[64], luigi_oam_next[64];  // coop.c
extern int luigi_nspr, luigi_nspr_next;
const u8 *follower_colors(void), *leader_colors(void);

// ---- CPU state ----
u8 A, X, Y, S = 0xFD, C, Z, I = 1, D, V, N;
u16 pc;
i64 cyc = 7, cyc_target;
u8 ram[0x800];
#define MAXBANKS 32
int prg_bank, trace;
const u8 (*PRG)[0x8000];
static const u8 *CHR_ROM;
static int nmi_pending;
static i64 nmi_cyc;  // earliest instruction-end cycle at which a pending NMI is taken
u8 frame_irq, dmc_irq;             // APU interrupt sources (apu.c)
static u8 apu_rd(void);
static void apu_wr(u16 a, u8 v);
static u8 miss[MAXBANKS][0x8000], ram_miss[0x800][256];  // ram_miss[addr][opcode]
static long interp_rom, interp_ram;  // instructions that fell back to the interpreter

// ---- PPU state ----
static u8 chr[0x2000], ntram[0x800], *nt[4], pal[32], oam[256];
static u8 ctrl, mask, status, oamaddr, wlatch, finex, rdbuf;
static u16 va, ta;                // "loopy" v and t scroll registers
static i64 line_dot0, s0_dot = -1; // absolute dot of current line start / of the sprite-0 hit
static int odd_frame, cur_line, stage;  // stage: which of the current line's events are done
static int frame_no, log_from = -1, log_to = -1;  // PPULOG=a-b: log PPU/mapper access for frames a..b
#define LOG(...) if (frame_no >= log_from && frame_no <= log_to) fprintf(stderr, "f%d L%3d d%4lld  ", frame_no, cur_line, (long long)(cyc * 3 - line_dot0)), fprintf(stderr, __VA_ARGS__)
static void ppu_run(i64 now);
static u32 fb[240 * 256];
static u8 pad_state, pad_shift, pad_strobe, pad2_state, pad2_shift;

static const u32 NESPAL[64] = {
  0x666666,0x002A88,0x1412A7,0x3B00A4,0x5C007E,0x6E0040,0x6C0600,0x561D00,0x333500,0x0B4800,0x005200,0x004F08,0x00404D,0,0,0,
  0xADADAD,0x155FD9,0x4240FF,0x7527FE,0xA01ACC,0xB71E7B,0xB53120,0x994E00,0x6B6D00,0x388700,0x0C9300,0x008F32,0x007C8D,0,0,0,
  0xFFFEFF,0x64B0FF,0x9290FF,0xC676FF,0xF36AFF,0xFE6ECC,0xFE8170,0xEA9E22,0xBCBE00,0x88D800,0x5CE430,0x45E082,0x48CDDE,0x4F4F4F,0,0,
  0xFFFEFF,0xC0DFFF,0xD3D2FF,0xE8C8FF,0xFBC2FF,0xFEC4EA,0xFECCC5,0xF7D8A5,0xE4E594,0xCFEF96,0xBDF4AB,0xB3F3CC,0xB5EBF2,0xB8B8B8,0,0};

static void set_mirror(int a, int b, int c, int d) {
  nt[0] = ntram + a * 0x400; nt[1] = ntram + b * 0x400; nt[2] = ntram + c * 0x400; nt[3] = ntram + d * 0x400;
}
static int pidx(u16 a) { a &= 0x1f; return (a & 0x13) == 0x10 ? a & 0x0f : a; }
static u8 pmrd(u16 a) {
  a &= 0x3fff;
  if (a < 0x2000) return chr[a];
  if (a < 0x3f00) return nt[(a >> 10) & 3][a & 0x3ff];
  return pal[pidx(a)];
}
static void pmwr(u16 a, u8 v) {
  a &= 0x3fff;
  if (a < 0x2000) { if (!CHR_SIZE) chr[a] = v; }
  else if (a < 0x3f00) nt[(a >> 10) & 3][a & 0x3ff] = v;
  else pal[pidx(a)] = v;
}
void ppu_poke(u16 a, u8 v) { pmwr(a, v); }  // coop.c: write a tile straight into the nametable

static u8 ppu_rd(int r) {
  ppu_run(cyc * 3);
  if (r == 2) {
    u8 v = status & 0xA0;
    if (s0_dot >= 0 && cyc * 3 > s0_dot) v |= 0x40;
    if (v & 0xC0) LOG("rd $2002 = %02X pc=%04X\n", v, pc);
    status &= ~0x80; wlatch = 0;
    return v;
  }
  if (r == 4) return oam[oamaddr];
  if (r == 7) {
    u8 v;
    if ((va & 0x3fff) >= 0x3f00) { v = pmrd(va); rdbuf = pmrd(va - 0x1000); }
    else { v = rdbuf; rdbuf = pmrd(va); }
    va += ctrl & 4 ? 32 : 1;
    return v;
  }
  return 0;
}
static void ppu_wr(int r, u8 v) {
  ppu_run(cyc * 3);
  if (r != 7 && r != 4) LOG("wr $200%d = %02X  (v=%04X t=%04X w=%d)\n", r, v, va, ta, wlatch);
  switch (r) {
  case 0:
    if (!(ctrl & 0x80) && (v & 0x80) && (status & 0x80)) { nmi_pending = 1; nmi_cyc = cyc + 1; cyc_target = 0; }
    ctrl = v; ta = (ta & ~0x0C00) | ((v & 3) << 10); break;
  case 1: mask = v; break;
  case 3: oamaddr = v; break;
  case 4: oam[oamaddr++] = v; break;
  case 5:
    if (!wlatch) { ta = (ta & ~0x1f) | (v >> 3); finex = v & 7; }
    else ta = (ta & ~0x73E0) | ((v & 7) << 12) | ((v & 0xF8) << 2);
    wlatch ^= 1; break;
  case 6:
    if (!wlatch) ta = (ta & 0x00FF) | ((v & 0x3F) << 8);
    else { ta = (ta & 0xFF00) | v; va = ta; }
    wlatch ^= 1; break;
  case 7: pmwr(va, v); va += ctrl & 4 ? 32 : 1; break;
  }
}

u8 rd(u16 a) {
  if (a < 0x2000) return ram[a & 0x7ff];
  if (a >= 0x8000) return PRG[prg_bank][a & 0x7fff];
  if (a < 0x4000) return ppu_rd(a & 7);
  if (a == 0x4016) {
    u8 b = pad_strobe ? pad_state & 1 : pad_shift & 1;
    if (!pad_strobe) pad_shift = (pad_shift >> 1) | 0x80;
    return 0x40 | b;
  }
  if (a == 0x4017) {  // controller 2
    u8 b = pad_strobe ? pad2_state & 1 : pad2_shift & 1;
    if (!pad_strobe) pad2_shift = (pad2_shift >> 1) | 0x80;
    return 0x40 | b;
  }
  if (a >= 0x6000) return playground_rd(a);  // playground.c's level data
  return a == 0x4015 ? apu_rd() : 0x40;
}
int start_level;  // --level N; read by the mod at bank 0 $8321 (mods.txt)
static int watch[8], nwatch;
void wr(u16 a, u8 v) {
  for (int i = 0; i < nwatch; i++) if (a == watch[i] && ram[a & 0x7ff] != v)
    fprintf(stderr, "f%d write $%04X: %02X -> %02X  at bank %d pc~%04X  A=%02X X=%02X Y=%02X\n", frame_no, a, ram[a & 0x7ff], v, prg_bank, pc, A, X, Y);
  if (a < 0x2000) { ram[a & 0x7ff] = v; return; }
  if (a < 0x4000) { ppu_wr(a & 7, v); return; }
  if (a >= 0x8000) {
    if (MAPPER == 7) {
      ppu_run(cyc * 3);  // the write also picks the nametable, mid-frame
      prg_bank = (v & 7) % NBANKS;
      int s = (v >> 4) & 1; set_mirror(s, s, s, s);
      LOG("mapper = %02X (bank %d, nametable %d)\n", v, v & 7, s);
      cyc_target = 0;  // leave the current bank's function after this instruction
    } else if (MAPPER == 2) {  // UxROM: picks the 16K bank at $8000; $C000 stays the last bank
      prg_bank = v % NBANKS;   // ponytail: bus conflicts (value ANDed with the ROM byte) not modelled
      cyc_target = 0;
    } else if (MAPPER == 3 && CHR_SIZE) {  // CNROM: picks the 8K graphics bank
      ppu_run(cyc * 3);
      memcpy(chr, CHR_ROM + (v % (CHR_SIZE / 0x2000)) * 0x2000, 0x2000);
    }
    return;
  }
  if (a == 0x4014) { ppu_run(cyc * 3);
    luigi_nspr = luigi_nspr_next; memcpy(luigi_oam, luigi_oam_next, sizeof luigi_oam); luigi_nspr_next = 0;  /* coop.c */ for (int i = 0; i < 256; i++) oam[(u8)(oamaddr + i)] = rd(v << 8 | i); cyc += 513 + (cyc & 1); return; }
  if (a == 0x4016) { if (pad_strobe && !(v & 1)) { pad_shift = pad_state; pad2_shift = pad2_state; } pad_strobe = v & 1; }
  else if (a >= 0x4000 && a <= 0x4017) apu_wr(a, v);
}
static void dump_misses(void) {
  unsigned b, a, op, any = 0;
  char line[64];
  for (b = 0; b < MAXBANKS; b++) for (a = 0; a < 0x8000; a++) any |= miss[b][a];
  for (a = 0; a < 0x800; a++) for (op = 0; op < 256; op++) any |= ram_miss[a][op];
  if (!any) return;
  FILE *f = fopen(MISSES, "r");  // merge with earlier runs so the file stays a set
  if (f) {
    while (fgets(line, sizeof line, f)) {
      if (sscanf(line, "R %x %x", &a, &op) == 2) { if (a < 0x800 && op < 256) ram_miss[a][op] = 1; }
      else if (sscanf(line, "%u %x", &b, &a) == 2 && b < MAXBANKS && a >= 0x8000 && a < 0x10000) miss[b][a & 0x7fff] = 1;
    }
    fclose(f);
  }
  if (!(f = fopen(MISSES, "w"))) return;
  for (b = 0; b < MAXBANKS; b++) for (a = 0; a < 0x8000; a++) if (miss[b][a]) fprintf(f, "%u %04X\n", b, a + 0x8000);
  for (a = 0; a < 0x800; a++) for (op = 0; op < 256; op++) if (ram_miss[a][op]) fprintf(f, "R %04X %02X\n", a, op);
  fclose(f);
}
void die(u8 op) {
  fflush(stdout);
  fprintf(stderr, "unsupported opcode %02X at bank %d pc %04X\n", op, prg_bank, pc);
  dump_misses(); exit(1);
}

#include "apu.c"
#include "coop.c"
#include "playground.c"
#include "cheats.c"
#include "save.c"
#include "replay.c"

static int force_interp;
static void cpu_run(i64 target) {
  while (cyc < target) {
    // The CPU samples the IRQ line one cycle before an instruction ends, so an IRQ raised on
    // the final cycle waits for the next instruction: catch the APU up to cyc-1 only.
    while (apu_cyc < cyc - 1) apu_cycle();
    if (nmi_pending && cyc >= nmi_cyc) {
      nmi_pending = 0;
      PUSH(pc >> 8); PUSH(pc); PUSH(getp()); I = 1; pc = rd16(0xFFFA); cyc += 7;
    } else if ((frame_irq | dmc_irq) && !I) {
      PUSH(pc >> 8); PUSH(pc); PUSH(getp()); I = 1; pc = rd16(0xFFFE); cyc += 7;
    }
    cyc_target = nmi_pending && nmi_cyc < target ? nmi_cyc : target;
    if (!fc_mode && !fc_inh && !frame_irq && !I) {  // stop where the APU frame IRQ will fire
      i64 at = apu_cyc + (29828 - fc) + 2;
      if (at > cyc && at < target) cyc_target = at;
    }
    if (force_interp || (pc >= 0x8000 ? bank_fn[prg_bank]() : pc < 0x800 ? run_ram() : 1)) {  // 1 = pc is not compiled
      if (cyc >= target) break;
      if (force_interp) ;
      else if (pc >= 0x8000) { miss[prg_bank][pc & 0x7fff] = 1; interp_rom++; }
      else { if (pc < 0x800) ram_miss[pc][ram[pc]] = 1; interp_ram++; }
      interp_step();
    }
  }
}
static void run_dot(i64 d) { cpu_run((d + 2) / 3); }

static void render_line(int y, i64 dot0) {
  u8 bg[256 + 8] = {0}, sp[256] = {0}, info[256] = {0};  // info: bit0 behind bg, bit1 sprite 0
  if (mask & 8) {
    u16 v = va, pt = (ctrl & 0x10) << 8;
    int fy = (v >> 12) & 7;
    for (int t = 0; t < 33; t++) {
      u8 *n = nt[(v >> 10) & 3];
      u16 tile = pt | n[v & 0x3ff] << 4 | fy;
      u8 hi = ((n[0x3c0 | ((v >> 4) & 0x38) | ((v >> 2) & 7)] >> (((v >> 4) & 4) | (v & 2))) & 3) << 2;
      u8 p0 = chr[tile], p1 = chr[tile | 8];
      for (int b = 0; b < 8; b++) {
        int px = t * 8 + b - finex;
        u8 c = ((p0 >> (7 - b)) & 1) | (((p1 >> (7 - b)) & 1) << 1);
        if (px >= 0 && px < 256 && c) bg[px] = hi | c;
      }
      if ((v & 0x1f) == 0x1f) v ^= 0x41f; else v++;
    }
    if (!(mask & 2)) memset(bg, 0, 8);
  }
  if (mask & 0x10) {
    int h = ctrl & 0x20 ? 16 : 8, n = 0, sel[8];
    for (int i = 0; i < luigi_nspr; i++) {  // coop.c: Luigi's extra sprite slots, below the game's
      u8 *o = luigi_oam + i * 4;
      int r = y - 1 - o[0];
      if (r < 0 || r >= 8) continue;  // the player is drawn from 8x8 sprites
      if (o[2] & 0x80) r = 7 - r;
      u16 a = ((ctrl & 8) << 9) | (o[1] << 4) | r;
      for (int b = 0; b < 8; b++) {
        int bit = o[2] & 0x40 ? b : 7 - b, px = o[3] + b;
        u8 c = ((chr[a] >> bit) & 1) | (((chr[a + 8] >> bit) & 1) << 1);
        if (!c || px > 255 || (px < 8 && !(mask & 4))) continue;
        sp[px] = 0x50 | ((o[2] & 3) << 2) | c;  // his palette bits: the star flash
        info[px] = (o[2] >> 5) & 1;
      }
    }
    for (int i = 0; i < 64; i++) {
      int r = y - 1 - oam[i * 4];
      if (r < 0 || r >= h) continue;
      if (n == 8) { status |= 0x20; break; }
      sel[n++] = i;
    }
    while (n--) {  // highest index first, so the lowest index ends up on top
      u8 *o = oam + sel[n] * 4;
      int r = y - 1 - o[0];
      if (o[2] & 0x80) r = h - 1 - r;
      u16 a = h == 16 ? ((o[1] & 1) << 12) | (((o[1] & 0xfe) + (r >> 3)) << 4) | (r & 7)
                      : ((ctrl & 8) << 9) | (o[1] << 4) | r;
      for (int b = 0; b < 8; b++) {
        int bit = o[2] & 0x40 ? b : 7 - b, px = o[3] + b;
        u8 c = ((chr[a] >> bit) & 1) | (((chr[a + 8] >> bit) & 1) << 1);
        if (!c || px > 255 || (px < 8 && !(mask & 4))) continue;
        sp[px] = 0x10 | ((o[2] & 3) << 2) | c;
        info[px] = ((o[2] >> 5) & 1) | (sel[n] == 0 ? 2 : 0);
      }
    }
  }
  for (int x = 0; x < 256; x++) {
    u8 b = bg[x], s = sp[x];
    if (s && b && (info[x] & 2) && x != 255 && s0_dot < 0) s0_dot = dot0 + x + 2;
    int show_s = s && (!b || !(info[x] & 1));
    const u8 *lc;  // coop.c: the follower in his own colours, the leader (sprite palette 0) in his
    u8 ci = show_s && (s & 0x4c) == 0x40 ? follower_colors()[s & 3]
          : show_s && (s & 0x1c) == 0x10 && (lc = leader_colors()) ? lc[s & 3] : pal[show_s ? s & 0x1f : b];
    fb[y * 256 + x] = NESPAL[ci & 0x3f];
  }
}

// Catch the PPU up: perform, in order, every per-line event that happened before dot `now`.
// Every register access calls this first, so an event and a write that are a few dots apart
// are applied in the order they happened on hardware even though the CPU runs whole
// instructions (this game enables rendering at dot 266 of a line, just after the dot-256 step).
static void ppu_run(i64 now) {
  for (;;) {
    int vis = cur_line < 240, pre = cur_line == 261, ren = (vis || pre) && (mask & 0x18);
    switch (stage) {
    case 0:  // dot 0: draw the line from the state it starts with
      if (line_dot0 >= now) return;
      if (vis) render_line(cur_line, line_dot0);
      break;
    case 1:  // dot 1: vblank flag rises (line 241) / flags clear (pre-render line)
      if (line_dot0 + 1 >= now) return;
      if (cur_line == 241) {
        status |= 0x80;
        // The CPU samples NMI before an instruction's last cycle: the edge (in the cycle holding
        // this dot) must be seen by the second-to-last cycle, else one more instruction runs first.
        if (ctrl & 0x80) { nmi_pending = 1; nmi_cyc = (line_dot0 + 1) / 3 + 2; cyc_target = 0; }
      }
      if (pre) { status &= ~0xA0; s0_dot = -1; }
      break;
    case 2:  // dot 256: step to the next pixel row
      if (line_dot0 + 256 >= now) return;
      if (ren) {
        if ((va & 0x7000) != 0x7000) va += 0x1000;
        else {
          int cy = (va >> 5) & 31;
          va &= ~0x7000;
          if (cy == 29) { cy = 0; va ^= 0x800; } else cy = (cy + 1) & 31;
          va = (va & ~0x3E0) | (cy << 5);
        }
      }
      break;
    case 3:  // dot 257: reload horizontal scroll
      if (line_dot0 + 257 >= now) return;
      if (ren) va = (va & ~0x41F) | (ta & 0x41F);
      break;
    case 4:  // dot 304: pre-render line reloads vertical scroll
      if (line_dot0 + 304 >= now) return;
      if (pre && ren) va = (va & ~0x7BE0) | (ta & 0x7BE0);
      break;
    case 5: {  // end of line; odd frames are one dot short when rendering
      int len = pre && odd_frame && ren ? 340 : 341;
      if (line_dot0 + len >= now) return;
      line_dot0 += len; stage = -1;
      if (++cur_line == 262) { cur_line = 0; odd_frame ^= 1; frame_no++; }
    }
    }
    stage++;
  }
}

static void frame(void) {
  rec_frame();  // replay.c: the replay's controllers, or these written to the recording
  for (int f = frame_no; frame_no == f;) {
    // run the CPU a line at a time; stop just after the vblank dot so the NMI is taken promptly
    i64 stop = cur_line == 241 && stage <= 1 ? line_dot0 + 2 : line_dot0 + 342;
    run_dot(stop);
    ppu_run(cyc * 3);
  }
  apu_run();
}

static void write_bmp(const char *path) {
  u8 h[54] = {'B', 'M', 0, 0, 0, 0, 0, 0, 0, 0, 54, 0, 0, 0, 40, 0, 0, 0, 0, 1, 0, 0, 240, 0, 0, 0, 1, 0, 24};
  FILE *f = fopen(path, "wb");
  if (!f) { perror(path); exit(1); }
  fwrite(h, 1, 54, f);
  for (int y = 239; y >= 0; y--) for (int x = 0; x < 256; x++) {
    u32 c = fb[y * 256 + x]; u8 p[3] = {c, c >> 8, c >> 16}; fwrite(p, 1, 3, f);
  }
  fclose(f);
}

// The ROM is looked for at --rom PATH, else ~/Library/Application Support/recomp/NAME.nes,
// else the path it was built from. Returns NULL on success, else a message for the user.
static const char *load_rom(const char *arg) {
  static char msg[2048], sup[1024];
  snprintf(sup, sizeof sup, "%s/Library/Application Support/recomp/" NAME ".nes", getenv("HOME") ? getenv("HOME") : ".");
  const char *paths[] = {arg ? arg : sup, arg ? NULL : ROM_DEFAULT};
  FILE *f = NULL;
  for (int i = 0; i < 2 && !f; i++) if (paths[i]) f = fopen(paths[i], "rb");
  if (!f) {
    snprintf(msg, sizeof msg, "No ROM file found.\n\nThis program contains the game's code but not its data. "
             "Copy your own " NAME " ROM (.nes) to:\n\n%s\n\nor start with --rom PATH.", sup);
    return msg;
  }
  u8 *buf = malloc(ROM_SIZE + 0x8000);
  size_t n = fread(buf, 1, ROM_SIZE + 1, f);
  fclose(f);
  u64 h = 0xcbf29ce484222325ull;
  for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 0x100000001b3ull;
  if (n != (size_t)ROM_SIZE || h != ROM_HASH) {
    snprintf(msg, sizeof msg, "That ROM file is not the version this program was built from "
             "(%zu bytes, expected %d; the contents differ). The compiled code only matches that exact file.", n, ROM_SIZE);
    return msg;
  }
  CHR_ROM = buf + PRG_OFF + PRG_LEN;
  if (NBANKS > MAXBANKS) return "ROM has more PRG banks than this runtime supports";
  if (MAPPER == 2 || PRG_LEN == 0x4000) {
    // 32K views: UxROM = 16K bank i + the fixed last 16K; NROM-128 = its one 16K bank, mirrored
    u8 *p = malloc(NBANKS * 0x8000);
    for (int i = 0; i < NBANKS; i++) {
      memcpy(p + i * 0x8000, buf + PRG_OFF + i * 0x4000, 0x4000);
      memcpy(p + i * 0x8000 + 0x4000, buf + PRG_OFF + PRG_LEN - 0x4000, 0x4000);
    }
    PRG = (const u8 (*)[0x8000])p;
  } else PRG = (const u8 (*)[0x8000])(buf + PRG_OFF);
  return NULL;
}

// Power-on state once the ROM is loaded.
static void boot(void) {
  apu_init();
  if (CHR_SIZE) memcpy(chr, CHR_ROM, CHR_SIZE < 0x2000 ? CHR_SIZE : 0x2000);
  if (MIRROR_V) set_mirror(0, 1, 0, 1); else set_mirror(0, 0, 1, 1);
  pc = rd16(0xFFFC);
}

#ifndef RT_LIB
int main(int argc, char **argv) {
  int frames = 0, start = -1;
  int start_at = 0;
  const char *record = NULL, *replay = NULL;
  const char *shot = NULL, *keys_arg = NULL, *keys2_arg = NULL, *wav = NULL, *poke = NULL, *ramdump = NULL, *exec1 = NULL, *rom_arg = NULL, *jump = NULL;
  for (int i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "--trace")) trace = 1;
    else if (!strcmp(argv[i], "--interp")) force_interp = 1;
    else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--level") && i + 1 < argc) start_level = atoi(argv[++i]);
    else if (!strcmp(argv[i], "--pc") && i + 1 < argc) start = strtol(argv[++i], NULL, 16);
    else if (!strcmp(argv[i], "--shot") && i + 1 < argc) shot = argv[++i];
    else if (!strcmp(argv[i], "--keys") && i + 1 < argc) keys_arg = argv[++i];
    else if (!strcmp(argv[i], "--keys2") && i + 1 < argc) keys2_arg = argv[++i];
    else if (!strcmp(argv[i], "--rom") && i + 1 < argc) rom_arg = argv[++i];
    else if (!strcmp(argv[i], "--wav") && i + 1 < argc) wav = argv[++i];
    else if (!strcmp(argv[i], "--poke") && i + 1 < argc) poke = argv[++i];
    else if (!strcmp(argv[i], "--ramdump") && i + 1 < argc) ramdump = argv[++i];
    else if (!strcmp(argv[i], "--exec1") && i + 1 < argc) exec1 = argv[++i];
    else if (!strcmp(argv[i], "--jump") && i + 1 < argc) jump = argv[++i];
    else if (!strcmp(argv[i], "--cheats") && i + 1 < argc) cheats_parse(argv[++i]);
    else if (!strcmp(argv[i], "--save") && i + 1 < argc) save_load(argv[++i]);
    else if (!strcmp(argv[i], "--bot")) coop_bot = 1;
    else if (!strcmp(argv[i], "--record") && i + 1 < argc) record = argv[++i];
    else if (!strcmp(argv[i], "--replay") && i + 1 < argc) replay = argv[++i];
    else if (!strcmp(argv[i], "--start") && i + 1 < argc) start_at = atoi(argv[++i]);
    else { fprintf(stderr, "usage: %s [--rom FILE.nes] [--level 1..12] [--frames N [--shot out.bmp] [--wav out.wav] [--keys FRAME:HEXMASK,...] [--keys2 ...]\n"
            "          [--poke F1-F2:HEXADDR:HEXVAL,...] [--ramdump out.bin]] [--interp] [--trace] [--pc HEX] [--cheats \"lives=1 world=4 level=2 time=1 fire=1\"] [--save FILE] [--bot]\n"
            "          [--record FILE] [--replay FILE [--start FRAME]] (replay window: Space pauses, . steps a frame)\n", argv[0]); return 2; }
  }
  const char *err = load_rom(rom_arg);
  if (err) {
    fprintf(stderr, "%s\n", err);
    if (!frames && !exec1) SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "ROM needed", err, NULL);
    return 1;
  }
  boot();
  if (replay && (err = replay_open(replay))) { fprintf(stderr, "%s: %s\n", replay, err); return 1; }
  if (record) rec_start(record);  // after the options: the save and --bot are in its header
  if (getenv("WATCH")) for (const char *q = getenv("WATCH"); nwatch < 8 && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") watch[nwatch++] = strtol(q, NULL, 16);
  FILE *ramlog = getenv("RAMLOG") ? fopen(getenv("RAMLOG"), "wb") : NULL;
  if (getenv("PPULOG")) sscanf(getenv("PPULOG"), "%d-%d", &log_from, &log_to);
  if (start >= 0) pc = start;

  if (exec1) {  // test aid: --exec1 BANK:PC:X:ADDR=VAL,... runs ONE compiled instruction and prints the result
    unsigned b, a, x, n = 0, ra, rv;
    if (sscanf(exec1, "%u:%x:%x:%n", &b, &a, &x, &n) < 3) return 2;
    for (const char *q = exec1 + n; sscanf(q, "%x=%x", &ra, &rv) == 2; q = strchr(q, ',') + 1) { ram[ra & 0x7ff] = rv; if (!strchr(q, ',')) break; }
    prg_bank = b; pc = a; X = x; cyc_target = cyc + 1;
    int missed = bank_fn[prg_bank]();
    printf("compiled=%d pc=%04X A=%02X X=%02X N=%d Z=%d cycles=%lld ram[0D]=%02X ram[11]=%02X ram[12]=%02X\n", !missed, pc, A, X, N, Z, (long long)cyc - 7, ram[0x0D], ram[0x11], ram[0x12]);
    return 0;
  }
  if (frames) {  // headless
    FILE *wf = NULL; u32 wav_n = 0;
    if (wav) {
      u8 h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,
                  APU_HZ & 255, APU_HZ >> 8, 0, 0, (APU_HZ * 2) & 255, ((APU_HZ * 2) >> 8) & 255, (APU_HZ * 2) >> 16, 0,
                  2,0,16,0,'d','a','t','a',0,0,0,0};
      if (!(wf = fopen(wav, "wb"))) { perror(wav); return 1; }
      fwrite(h, 1, 44, wf);
    }
    // --keys "120:08,130:00": from frame 120 hold Start, from 130 release. Mask bits: A B Sel Start U D L R.
    for (int f = 0; f < frames; f++) {
      while (keys_arg && *keys_arg && atoi(keys_arg) <= f) {
        const char *c = strchr(keys_arg, ':');
        if (!c) { fprintf(stderr, "bad --keys\n"); return 2; }
        pad_state = strtol(c + 1, NULL, 16);
        keys_arg = strchr(c, ',') ? strchr(c, ',') + 1 : "";
      }
      while (keys2_arg && *keys2_arg && atoi(keys2_arg) <= f) {  // same format, controller 2
        const char *c = strchr(keys2_arg, ':');
        if (!c) { fprintf(stderr, "bad --keys2\n"); return 2; }
        pad2_state = strtol(c + 1, NULL, 16);
        keys2_arg = strchr(c, ',') ? strchr(c, ',') + 1 : "";
      }
      // debugging aid: --poke "1400-2300:000D:02" forces RAM[$000D]=2 before each of those frames
      for (const char *q = poke; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") {
        int f1, f2; unsigned a, v;
        if (sscanf(q, "%d-%d:%x:%x", &f1, &f2, &a, &v) != 4) { fprintf(stderr, "bad --poke\n"); return 2; }
        if (f >= f1 && f <= f2) ram[a & 0x7ff] = v;
      }
      // test aid: --jump "2500:0:D387" sets bank and pc before that frame (test.sh uses it to
      // enter the game's own level-complete code without having to beat a level)
      if (jump) {
        int jf; unsigned jb, ja;
        if (sscanf(jump, "%d:%u:%x", &jf, &jb, &ja) != 3) { fprintf(stderr, "bad --jump\n"); return 2; }
        if (f == jf) { prg_bank = jb % NBANKS; pc = ja; }
      }
      frame();
      if (ramlog) fwrite(ram, 1, sizeof ram, ramlog);
      if (getenv("PCLOG")) fprintf(stderr, "frame %d end pc=%04X bank=%d mask=%02X ctrl=%02X\n", f, pc, prg_bank, mask, ctrl);
      if (wf) fwrite(abuf, 2, an, wf), wav_n += an;
      an = 0;
    }
    if (wf) {  // patch the two size fields now that the length is known
      u32 d = wav_n * 2, r = d + 36;
      fseek(wf, 4, SEEK_SET); fwrite(&r, 4, 1, wf); fseek(wf, 40, SEEK_SET); fwrite(&d, 4, 1, wf); fclose(wf);
    }
    if (shot) write_bmp(shot);
    if (ramdump) { FILE *rf = fopen(ramdump, "wb"); if (rf) { fwrite(ram, 1, sizeof ram, rf); fclose(rf); } }
    fprintf(stderr, "interpreted: %ld ROM instructions (not yet compiled), %ld from RAM; %lld CPU cycles total\n", interp_rom, interp_ram, (long long)cyc);
    dump_misses();
    return 0;
  }
  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
  SDL_Window *w = SDL_CreateWindow("recomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 768, 720, SDL_WINDOW_RESIZABLE);
  SDL_Renderer *r = SDL_CreateRenderer(w, -1, 0);
  SDL_RenderSetLogicalSize(r, 256, 240);
  SDL_Texture *t = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, 256, 240);
  // A=X, B=Z, Select=RShift, Start=Return, then Up Down Left Right
  static const int keys[8] = {SDL_SCANCODE_X, SDL_SCANCODE_Z, SDL_SCANCODE_RSHIFT, SDL_SCANCODE_RETURN,
                              SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT};
  // player 2: A=G, B=F, Select=1, Start=2, then W S A D
  static const int keys2[8] = {SDL_SCANCODE_G, SDL_SCANCODE_F, SDL_SCANCODE_1, SDL_SCANCODE_2,
                               SDL_SCANCODE_W, SDL_SCANCODE_S, SDL_SCANCODE_A, SDL_SCANCODE_D};
  SDL_AudioSpec want = {.freq = APU_HZ, .format = AUDIO_S16SYS, .channels = 1, .samples = 1024};
  SDL_AudioDeviceID dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
  if (dev) SDL_PauseAudioDevice(dev, 0); else fprintf(stderr, "no audio: %s\n", SDL_GetError());
  double next = SDL_GetTicks();
  int paused = 0, step = 0;
  for (;;) {
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
      if (e.type == SDL_QUIT) { dump_misses(); return 0; }
      if (replay && e.type == SDL_KEYDOWN && !e.key.repeat) {  // replay: Space pauses, . steps a frame
        if (e.key.keysym.sym == SDLK_SPACE) paused ^= 1;
        if (e.key.keysym.sym == SDLK_PERIOD) step = paused = 1;
      }
    }
    if (paused && !step) { SDL_Delay(16); next = SDL_GetTicks(); continue; }
    step = 0;
    const u8 *k = SDL_GetKeyboardState(NULL);
    pad_state = pad2_state = 0;
    for (int i = 0; i < 8; i++) { pad_state |= k[keys[i]] << i; pad2_state |= k[keys2[i]] << i; }
    frame();
    if (frame_no < start_at) { an = 0; next = SDL_GetTicks(); continue; }  // --start: skip ahead unseen
    if (dev) {
      if (SDL_GetQueuedAudioSize(dev) > APU_HZ * 2 / 4) SDL_ClearQueuedAudio(dev);  // >0.25s behind: resync
      SDL_QueueAudio(dev, abuf, an * 2);
    }
    an = 0;
    SDL_UpdateTexture(t, NULL, fb, 256 * 4);
    SDL_RenderClear(r); SDL_RenderCopy(r, t, NULL, NULL); SDL_RenderPresent(r);
    next += 1000.0 / 60.0988;
    double now = SDL_GetTicks();
    if (next > now) SDL_Delay((u32)(next - now)); else if (now - next > 100) next = now;
  }
}
#endif
