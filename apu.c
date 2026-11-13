// NES sound chip (APU): 2 pulse, triangle, noise, DMC sample channel, frame counter + IRQs.
// Included by rt.c. Runs in catch-up: apu_run() advances it one CPU cycle at a time to `cyc`.
//
// ponytail: DMC sample fetches do not steal CPU cycles (real hardware stalls the CPU up to
//           4 cycles per byte). Battletoads never starts a DMC sample: its code has no write
//           to $4012/$4013 and only ever stores $00 or $0F to $4015 (see build/battletoads.lst),
//           so this cannot affect it. Add the stall before trusting timing in a game that does.
#define APU_HZ 44100
#define CPU_HZ 1789773

static const u8 LEN_TBL[32] = {10, 254, 20, 2, 40, 4, 80, 6, 160, 8, 60, 10, 14, 12, 26, 14,
                               12, 16, 24, 18, 48, 20, 96, 22, 192, 24, 72, 26, 16, 28, 32, 30};
static const u8 DUTY[4][8] = {{0, 1, 0, 0, 0, 0, 0, 0}, {0, 1, 1, 0, 0, 0, 0, 0}, {0, 1, 1, 1, 1, 0, 0, 0}, {1, 0, 0, 1, 1, 1, 1, 1}};
static const u16 NOISE_PER[16] = {4, 8, 16, 32, 64, 96, 128, 160, 202, 254, 380, 508, 762, 1016, 2034, 4068};
static const u16 DMC_RATE[16] = {428, 380, 340, 320, 286, 254, 226, 214, 190, 160, 142, 128, 106, 84, 72, 54};

// ch[0], ch[1] = pulse, ch[2] = noise (shares envelope + length counter), ch[3] = triangle (length only)
static struct ch {
  u8 en, halt, cvol, vol, len, estart, ediv, edecay, len_pre;
  u8 duty, step, sw_en, sw_per, sw_neg, sw_sh, sw_reload, sw_div;
  u16 period, timer;
} ch[4];
static u8 tri_lin, tri_lin_val, tri_lin_reload, noise_mode, noise_idx;
static u16 noise_sh = 1;
static struct {
  u8 irq_en, loop, rate, level, buf, buf_full, shift, bits, silence;
  u16 addr, len, cur, bytes, timer;
} dmc = {.bits = 8, .silence = 1};
static u8 fc_mode, fc_inh, fc_delay, fc_kick;
static int fc;
static i64 apu_cyc, half_cyc = -1;  // half_cyc: cycle on which length counters were last clocked
static float pulse_tbl[31], tnd_tbl[203], acc, hp;
static int accn, frac, an;
static int16_t abuf[4096];

static void apu_init(void) {
  for (int i = 1; i < 31; i++) pulse_tbl[i] = 95.52f / (8128.0f / i + 100);
  for (int i = 1; i < 203; i++) tnd_tbl[i] = 163.67f / (24329.0f / i + 100);
}

static void quarter(void) {  // envelopes + triangle linear counter
  for (int i = 0; i < 3; i++) {
    struct ch *c = ch + i;
    if (c->estart) { c->estart = 0; c->edecay = 15; c->ediv = c->vol; }
    else if (c->ediv) c->ediv--;
    else { c->ediv = c->vol; if (c->edecay) c->edecay--; else if (c->halt) c->edecay = 15; }
  }
  if (tri_lin_reload) tri_lin = tri_lin_val; else if (tri_lin) tri_lin--;
  if (!ch[3].halt) tri_lin_reload = 0;
}
static int sweep_target(int i) {
  struct ch *c = ch + i;
  int d = c->period >> c->sw_sh;
  return c->sw_neg ? c->period - d - (i == 0) : c->period + d;
}
static void half(void) {  // length counters + sweeps
  half_cyc = apu_cyc;
  for (int i = 0; i < 4; i++) { ch[i].len_pre = ch[i].len; if (ch[i].len && !ch[i].halt) ch[i].len--; }
  for (int i = 0; i < 2; i++) {
    struct ch *c = ch + i;
    int t = sweep_target(i);
    if (!c->sw_div && c->sw_en && c->sw_sh && c->period >= 8 && t <= 0x7ff) c->period = t < 0 ? 0 : t;
    if (!c->sw_div || c->sw_reload) { c->sw_div = c->sw_per; c->sw_reload = 0; } else c->sw_div--;
  }
}

static void apu_cycle(void) {
  if (fc_kick && !--fc_kick) { quarter(); half(); }  // 5-step mode clocks once right away
  if (fc_delay && !--fc_delay) fc = 0;
  fc++;
  if (fc == 7457 || fc == 22371) quarter();
  else if (fc == 14913) { quarter(); half(); }
  else if (!fc_mode) {
    if (fc >= 29828 && !fc_inh) frame_irq = 1;
    if (fc == 29829) { quarter(); half(); }
    if (fc == 29830) fc = 0;
  } else {
    if (fc == 37281) { quarter(); half(); }
    if (fc == 37282) fc = 0;
  }

  if (apu_cyc & 1) for (int i = 0; i < 2; i++) {
    if (ch[i].timer) ch[i].timer--; else { ch[i].timer = ch[i].period; ch[i].step = (ch[i].step + 1) & 7; }
  }
  if (ch[3].timer) ch[3].timer--;
  else { ch[3].timer = ch[3].period; if (ch[3].len && tri_lin && ch[3].period >= 2) ch[3].step = (ch[3].step + 1) & 31; }
  if (ch[2].timer) ch[2].timer--;
  else {
    ch[2].timer = NOISE_PER[noise_idx] - 1;
    noise_sh = (noise_sh >> 1) | (((noise_sh ^ (noise_sh >> (noise_mode ? 6 : 1))) & 1) << 14);
  }
  if (!dmc.buf_full && dmc.bytes) {
    dmc.buf = rd(dmc.cur); dmc.buf_full = 1;
    dmc.cur = dmc.cur == 0xFFFF ? 0x8000 : dmc.cur + 1;
    if (!--dmc.bytes) { if (dmc.loop) { dmc.cur = dmc.addr; dmc.bytes = dmc.len; } else if (dmc.irq_en) dmc_irq = 1; }
  }
  if (dmc.timer) dmc.timer--;
  else {
    dmc.timer = DMC_RATE[dmc.rate] - 1;
    if (!dmc.silence) { if (dmc.shift & 1) { if (dmc.level <= 125) dmc.level += 2; } else if (dmc.level >= 2) dmc.level -= 2; }
    dmc.shift >>= 1;
    if (!--dmc.bits) {
      dmc.bits = 8;
      if (dmc.buf_full) { dmc.silence = 0; dmc.shift = dmc.buf; dmc.buf_full = 0; } else dmc.silence = 1;
    }
  }

  int p = 0;
  for (int i = 0; i < 2; i++) {
    struct ch *c = ch + i;
    if (c->len && c->period >= 8 && (c->sw_neg || sweep_target(i) <= 0x7ff) && DUTY[c->duty][c->step])
      p += c->cvol ? c->vol : c->edecay;
  }
  int tri = ch[3].step < 16 ? 15 - ch[3].step : ch[3].step - 16;
  int noise = (noise_sh & 1) || !ch[2].len ? 0 : ch[2].cvol ? ch[2].vol : ch[2].edecay;
  acc += pulse_tbl[p] + tnd_tbl[3 * tri + 2 * noise + dmc.level]; accn++;
  if ((frac += APU_HZ) >= CPU_HZ) {  // box-filter down to the output rate, then remove DC
    frac -= CPU_HZ;
    float s = acc / accn; acc = 0; accn = 0;
    hp += (s - hp) * 0.002f;
    if (an < 4096) abuf[an++] = (int16_t)((s - hp) * 30000);
  }
  apu_cyc++;
}
static void apu_run(void) { while (apu_cyc < cyc) apu_cycle(); }

static u8 apu_rd(void) {  // $4015
  // A read does not see what happens on its own cycle; a write (below) lands after it.
  while (apu_cyc < cyc - 1) apu_cycle();
  u8 v = (ch[0].len > 0) | (ch[1].len > 0) << 1 | (ch[3].len > 0) << 2 | (ch[2].len > 0) << 3 |
         (dmc.bytes > 0) << 4 | frame_irq << 6 | dmc_irq << 7;
  frame_irq = 0;
  return v;
}
static void apu_wr(u16 a, u8 v) {
  apu_run();
  if (a == 0x4015) {
    static const u8 order[4] = {0, 1, 3, 2};  // bit -> ch index (bit 2 = triangle, bit 3 = noise)
    for (int i = 0; i < 4; i++) if (!(ch[order[i]].en = (v >> i) & 1)) ch[order[i]].len = 0;
    if (!(v & 0x10)) dmc.bytes = 0; else if (!dmc.bytes) { dmc.cur = dmc.addr; dmc.bytes = dmc.len; }
    dmc_irq = 0;
    return;
  }
  if (a == 0x4017) {
    fc_mode = v >> 7; fc_inh = (v >> 6) & 1;
    if (fc_inh) frame_irq = 0;
    int odd = !(cyc & 1);  // a write on an odd APU clock takes effect one clock later
    fc_delay = 3 + odd; fc_kick = fc_mode ? 1 + odd : 0;
    return;
  }
  if (a >= 0x4010) {
    if (a == 0x4010) { dmc.irq_en = v >> 7; dmc.loop = (v >> 6) & 1; dmc.rate = v & 15; if (!dmc.irq_en) dmc_irq = 0; }
    else if (a == 0x4011) dmc.level = v & 0x7f;
    else if (a == 0x4012) dmc.addr = 0xC000 | v << 6;
    else if (a == 0x4013) dmc.len = v << 4 | 1;
    return;
  }
  static const u8 which[4] = {0, 1, 3, 2};  // $4000, $4004 pulse; $4008 triangle; $400C noise
  struct ch *c = ch + which[(a >> 2) & 3];
  int tri = c == ch + 3, noise = c == ch + 2;
  switch (a & 3) {
  case 0:
    if (tri) { c->halt = v >> 7; tri_lin_val = v & 0x7f; }
    else { c->duty = v >> 6; c->halt = (v >> 5) & 1; c->cvol = (v >> 4) & 1; c->vol = v & 15; }
    break;
  case 1:
    if (!tri && !noise) { c->sw_en = v >> 7; c->sw_per = (v >> 4) & 7; c->sw_neg = (v >> 3) & 1; c->sw_sh = v & 7; c->sw_reload = 1; }
    break;
  case 2:
    if (noise) { noise_mode = v >> 7; noise_idx = v & 15; } else c->period = (c->period & 0x700) | v;
    break;
  case 3:
    if (!noise) c->period = (c->period & 0xff) | (v & 7) << 8;
    // a reload on the very cycle the counter is clocked is ignored if it was non-zero
    if (c->en && !(half_cyc == apu_cyc - 1 && c->len_pre)) c->len = LEN_TBL[v >> 3];
    if (tri) tri_lin_reload = 1; else { c->estart = 1; if (!noise) c->step = 0; }
    break;
  }
}
