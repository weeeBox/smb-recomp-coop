// Shared between the runtime (rt.c) and the generated game code (build/NAME.gen.c).
// They are separate files so the huge generated functions compile unoptimised (seconds)
// while the runtime is optimised: clang's optimiser and register allocator go
// superlinear on a 15,000-case function (5+ minutes when it was all one file).
#include <stdint.h>
#include <stdio.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef int64_t i64; typedef uint64_t u64;

// defined by the generated file
extern const int NBANKS, MAPPER, MIRROR_V, CHR_SIZE, ROM_SIZE, PRG_OFF, PRG_LEN;
extern const u64 ROM_HASH;  // FNV-1a 64 of the ROM file the code was generated from
extern int (*const bank_fn[])(void);  // one per PRG bank; returns 1 when pc is not compiled
int run_ram(void);  // code the game runs from RAM ($0000-$07FF); returns 1 when not compiled
int interp_step(void);

// defined by rt.c
extern u8 A, X, Y, S, C, Z, I, D, V, N, ram[0x800], frame_irq, dmc_irq;
extern u16 pc;
extern i64 cyc, cyc_target;  // CPU cycles; 1 cycle = 3 PPU dots
extern int prg_bank, trace, start_level;
extern const u8 (*PRG)[0x8000];  // the game's data, read from the user's ROM file at startup
u8 rd(u16 a);
void wr(u16 a, u8 v);
void die(u8 op);

static inline u16 rd16(u16 a) { return rd(a) | rd(a + 1) << 8; }
static inline u16 rd16zp(u8 a) { return ram[a] | ram[(u8)(a + 1)] << 8; }
static inline u8 getp(void) { return C | Z << 1 | I << 2 | D << 3 | 0x20 | V << 6 | N << 7; }
// Leave the compiled code at once when interrupts get unmasked while one is waiting.
#define IRQCHK if ((frame_irq | dmc_irq) && !I) cyc_target = 0
static inline void setp(u8 p) { C = p & 1; Z = (p >> 1) & 1; I = (p >> 2) & 1; D = (p >> 3) & 1; V = (p >> 6) & 1; N = p >> 7; IRQCHK; }

#define PUSH(v) (ram[0x100 | S--] = (v))
#define PULL() (ram[0x100 | ++S])
#define NZ(v) (Z = !(v), N = (v) >> 7)
#define ADC(m_) { unsigned t = A + (m_) + C; V = ((~(A ^ (m_)) & (A ^ t)) >> 7) & 1; C = t >> 8; A = t; NZ(A); }
#define CMP(r, m_) { u8 t = (r) - (m_); C = (r) >= (m_); NZ(t); }
#define TR(a) if (trace) printf("%04X A:%02X X:%02X Y:%02X P:%02X SP:%02X CYC:%lld\n", (a), A, X, Y, getp(), S, (long long)cyc)

// coop.c (called from mods-smb.txt)
void coop_turn(void);
void coop_new_game(void);
void coop_collide(u16 addr);
void coop_platform(int small);
void coop_ride(int small);
void coop_lose_life(void);
void coop_weigh(void);
void coop_spring(int done);
void coop_scroll(void);
void coop_enemy_diff(void);
void coop_bloober(void);
void playground_frame(void);
void playground_area(void);
u8 playground_rd(u16 a);
void coop_pads(void);
extern int coop_bot;
void coop_frame(void);
void coop_loopback(void);
void coop_whirlpool(void);
void coop_timers(void);
void coop_draw(void);
void coop_fireball(int done);
void coop_firebar(void);
void coop_fireballs(void);
void coop_star(int cmp);
void coop_victory_walk(int setup);
void coop_name(void);
void ppu_poke(u16 a, u8 v);
void coop_thanks(void);
// cheats.c
void cheats_parse(const char *s);
void cheats_new_game(void);
void cheats_frame(void);
void save_load(const char *path);
void save_frame(void);
void save_new_game(void);
void rec_cheats(const char *s);
void rec_start(const char *path);
const char *replay_open(const char *path);
