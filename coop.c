// Simultaneous two-player mode for Super Mario Bros.: Mario and Luigi play at the same time.
// Included by rt.c. The game knows one player, kept in slot 0 of its object tables and in a set
// of player variables. Whoever is in slot 0 is the LEADER: the game drives him (camera, pipes,
// flagpole, victory walk) exactly as in the original. The other brother, the FOLLOWER, lives in
// fol[] here; the hooks in mods-smb.txt swap him into slot 0, run the game's own player routines
// on him, and swap him back out. Addresses and names are from doppelganger's SMB disassembly
// (tools/smbsyms.py).
//
// Rules:
// - Leader swap: when the follower touches something that starts a cutscene (a pipe, a vine, the
//   flagpole, Bowser's axe), the brothers trade places, so the one who touched it does it.
// - The camera follows the leader, but never scrolls on past the follower: a brother who runs
//   ahead waits at the right edge of the screen until the other catches up.
// - Each brother has his own lives. Any death goes as in the original game: the one who died
//   loses a life and the level restarts from its checkpoint (he leads from there). Either
//   brother losing his last life ends the game. Time running out costs both a life. The lives
//   screen shows both brothers' lives.
// - During the leader's cutscenes the follower stands still; after an area change he reappears
//   next to the leader, keeping his size. After Bowser's axe he walks to Toad with the leader.
// - When the screen's left edge, scrolling on with the leader, pushes the follower into a wall
//   or pipe, he joins the leader (reappears where the leader is).
// - The status bar names the leader; Toad and the princess thank him (he touched the axe).
//
// - Moving platforms carry the follower too, and the ones that react to a rider (falling,
//   balance and start-when-stepped-on platforms) react to either brother.
// - Springs bounce whichever brother landed on them (high bounce: A held, from his controller);
//   if both are on springs at once, the leader's goes first.
//
// - Fire bars hurt either brother, by his own star and injury timers.
//
// - Star power: each brother has his own star timer; the star music ends when the last star does,
//   and the follower flashes in the star colours like the leader. Reappearing, he keeps his own
//   timers (a new brother has none), but a death restart clears them as in the original game.
//
// - A piranha plant stays in its pipe while either brother is near it. Lakitu picks a brother
//   at random (the game's own random bits) when he first aims and again after each Spiny he
//   throws, and follows him. Every other enemy that aims (hammer bros, Bowser, Bullet Bills,
//   Bloobers) aims at the leader, as before.
//
// - Water: whirlpools pull the follower by the same rule as the leader. Air bubbles come from the
//   leader only: a lone leader keeps all 3 bubble slots busy, so any slot the follower took would
//   hold up the leader's next bubble.
//
// - Fire power: the brothers share the game's two fireballs. A brother throws into the next one
//   if it is free, by the game's own rules.

// Runs the 6502 subroutine at `addr` to its RTS, from inside a mod, and restores the CPU state.
// The return address pushed is $FFFF, so the RTS lands on $0000: that ends the call.
static void call6502(u16 addr) {
  u8 sa = A, sx = X, sy = Y, ss = S, sp = getp();
  u16 spc = pc;
  i64 st = cyc_target;
  PUSH(0xFF); PUSH(0xFF);
  pc = addr;
  while (pc != 0) {
    cyc_target = INT64_MAX;  // a frame event may have zeroed it to stop the outer loop: run on
    if (pc >= 0x8000 && !bank_fn[prg_bank]()) continue;
    if (pc == 0) break;
    interp_step();
  }
  A = sa; X = sx; Y = sy; S = ss; setp(sp); pc = spc;
  cyc_target = nmi_pending ? 0 : st;  // an NMI raised meanwhile is taken as soon as possible
}

// The player's variables: object slot 0 of the position/speed/collision tables, the physics and
// input bytes, size and power-up status, the player's frame and interval timers.
static const u16 SLICE[] = {
  0x0A, 0x0B, 0x0C, 0x0D,          // A_B_Buttons Up_Down_Buttons Left_Right_Buttons PreviousA_B_Buttons
  0x0E,                            // GameEngineSubroutine (the player's mode: 8 = normal control)
  0x1D, 0x33, 0x45, 0x57, 0x6D, 0x86, 0x9F, 0xB5, 0xCE,  // Player_State FacingDir MovingDir X_Speed PageLoc X_Position Y_Speed Y_HighPos Y_Position
  0x3AD, 0x3B8, 0x3C4, 0x3D0,      // Rel_XPos Rel_YPos SprAttrib OffscreenBits
  0x400, 0x416, 0x433,             // X_MoveForce YMF_Dummy Y_MoveForce
  0x450, 0x456, 0x484, 0x490, 0x499,  // MaximumLeft/RightSpeed StompChainCounter CollisionBits BoundBoxCtrl
  0x4AC, 0x4AD, 0x4AE, 0x4AF,      // player bounding box
  0x6D5, 0x6DB, 0x6FF,            // PlayerGfxOffset JumpspringForce Player_X_Scroll
  0x700, 0x701, 0x702, 0x703, 0x704, 0x705, 0x706, 0x707, 0x708, 0x709, 0x70A, 0x70B, 0x70C, 0x70D, 0x70E,  // speed, friction, jump, swim, anim, JumpspringAnimCtrl
  0x711,                           // FireballThrowingTimer (his throwing pose)
  0x714, 0x754, 0x755, 0x756,      // CrouchingFlag PlayerSize Player_Pos_ForScroll (set by his drawing) PlayerStatus
  0x75A,                           // NumberofLives: each brother has his own (1-ups go to who takes them)
  0x781, 0x782, 0x783, 0x785, 0x789,  // frame timers: anim, jump/swim, running, side collision, climb
  0x79E, 0x79F,                    // interval timers: InjuryTimer StarInvincibleTimer
  0x47D,                           // Whirlpool_Flag (in a whirlpool)
};
#define NSLICE (int)(sizeof SLICE / sizeof SLICE[0])
// Shared screen state the follower's turn must leave alone: he never scrolls the camera.
static const u16 SCREEN[] = {0x723, 0x775, 0x71A, 0x71B, 0x71C, 0x71D, 0x73D, 0x73F, 0x778, 0x795, 0x3A1};
#define NSCREEN (int)(sizeof SCREEN / sizeof SCREEN[0])

static u8 fol[NSLICE];     // the follower's copy of the player variables
static const u8 NAMES[2][5] = {{0x16, 0x0A, 0x1B, 0x12, 0x18}, {0x15, 0x1E, 0x12, 0x10, 0x12}};  // MARIO, LUIGI tiles
static int coop_on, luigi_leads, fol_in, away = 1, died;  // away: follower not in this area yet
static int fresh_lives;    // a new game: the follower starts with the leader's number of lives
static u8 pad1;            // controller 1 this frame (the game's copy gets the leader's pad)
// The computer plays Luigi (rt.c --bot; always offered on Android) in a two-player game until
// controller 2 presses anything. Its Luigi never costs the game anything: a death puts him back
// by Mario, and he never takes the lead (the pipe, vine, flagpole or axe is Mario's to take).
int coop_bot;
static int human2;          // controller 2 has been used this game: a person plays Luigi
static int bot_on(void) { return coop_bot && coop_on && !human2; }
u8 luigi_oam[64], luigi_oam_next[64];  // the follower's sprites (16 max), drawn by rt.c from extra OAM slots
int luigi_nspr, luigi_nspr_next;

static u8 *F(u16 a) {  // the follower's copy of the variable at address a
  for (int i = 0; i < NSLICE; i++) if (SLICE[i] == a) return &fol[i];
  abort();
}

static void swap(void) {
  for (int i = 0; i < NSLICE; i++) { u8 t = ram[SLICE[i]]; ram[SLICE[i]] = fol[i]; fol[i] = t; }
  fol_in ^= 1;
}

// Is the follower stuck in a wall: the middle of his body inside a block that stops him from the
// side (ground, a brick, ? block, pipe or stair, not a vine, the flagpole or a coin)? Read from
// the game's block buffer: 13 rows of 16 blocks from y=32 for each of two pages. The rule is the
// game's own side check (DoPlayerSideCheck): any block but empty, a sideways pipe's top ($1C,
// $6B), a climbable one (ClimbMTileUpperExt, by the block's top two bits), a hidden block
// ($5F, $60) or a coin ($C2, $C3).
static int solid_at(int x, int y) {
  static const u8 CLIMB[] = {0x24, 0x6D, 0x8A, 0xC6};
  if (y < 32 || y >= 240) return 0;
  u8 t = ram[(x & 0x100 ? 0x5D0 : 0x500) + ((y - 32) & 0xF0) + ((x >> 4) & 0x0F)];
  return t && t < CLIMB[t >> 6] && !strchr("\x1C\x6B\x5F\x60\xC2\xC3", t);
}
static int stuck(void) {  // his feet's row, and his head's when big (bounding box 0)
  int x = (*F(0x6D) << 8 | *F(0x86)) + 8, y = *F(0xCE);
  return *F(0xB5) == 1 && (solid_at(x, y + 24) || (*F(0x499) == 0 && solid_at(x, y + 8)));
}
static void to_leader(void) {  // the one place sure to be free: where the leader is
  *F(0x6D) = ram[0x6D]; *F(0x86) = ram[0x86]; *F(0xCE) = ram[0xCE]; *F(0xB5) = ram[0xB5];
  *F(0x57) = 0; *F(0x700) = 0;  // X speed, XSpeedAbsolute
}

// The follower takes the leader's place, 16 pixels behind him (on him if that is in a wall);
// small and blinking after a death.
static void respawn(int small) {
  u8 size = *F(0x754), status = *F(0x756), lives = *F(0x75A), hurt = *F(0x79E), star = *F(0x79F);  // his own, kept
  for (int i = 0; i < NSLICE; i++) fol[i] = ram[SLICE[i]];
  if (fresh_lives) { fresh_lives = 0; hurt = star = 0; } else *F(0x75A) = lives;  // (joining mid-level: the leader's lives, no timers)
  *F(0x79E) = hurt; *F(0x79F) = star;  // his InjuryTimer and star, not the leader's
  if (star > 4) ram[0xFB] = 0x40;      // (the new area's music has replaced the star music)
  int x = ram[0x6D] << 8 | ram[0x86];
  int left = ram[0x71A] << 8 | ram[0x71C];
  x = x - 16 < left + 8 ? left + 8 : x - 16;
  *F(0x86) = x & 0xFF; *F(0x6D) = x >> 8;  // Player_X_Position, Player_PageLoc
  *F(0x0E) = 8;                            // GameEngineSubroutine: normal control
  *F(0x9F) = 0; *F(0x57) = 0; *F(0x700) = 0; *F(0x70E) = 0;  // Y and X speed, XSpeedAbsolute, no spring: he starts standing
  if (small) { *F(0x754) = 1; *F(0x756) = 0; *F(0x79E) = 8; }  // small, InjuryTimer blinking
  else { *F(0x754) = size; *F(0x756) = status; }
  if (stuck()) to_leader();
}

static u8 snap[0x800];
// Runs `addr` as the follower. If that kills him, he takes the lead right there, so the game's
// own death follows: the death music, a life taken from his counter, the level restarting from
// its checkpoint. With `idle` (the leader's cutscene) his controller reads as nothing pressed,
// and a turn that would start anything but normal control (a pipe, the flagpole) or kill him
// is undone: he only falls and lands.
static void run_as_follower(u16 addr, int idle) {
  u8 scr[NSCREEN], before[NSLICE];
  memcpy(snap, ram, sizeof ram);
  memcpy(before, fol, sizeof fol);
  swap();
  for (int i = 0; i < NSCREEN; i++) scr[i] = ram[SCREEN[i]];
  u8 joy = ram[0x6FC];
  ram[0x6FC] = idle ? 0 : luigi_leads ? pad1 : ram[0x6FD];  // SavedJoypadBits <- his controller
  ram[0x723] = 1;                                // ScrollLock: his movement never scrolls
  call6502(addr);
  ram[0x6FC] = joy;
  for (int i = 0; i < NSCREEN; i++) ram[SCREEN[i]] = scr[i];
  // dead: killed (mode $0B), or fallen below the screen (not on a cloud bonus level, where that
  // is the way back down: the game changes the area for both)
  int dead = ram[0x0E] == 0x0B || ram[0x0E] == 0x06 || (ram[0xB5] >= 2 && !ram[0x743]);
  int lead = (ram[0x0E] >= 1 && ram[0x0E] <= 4) || (snap[0x770] == 1 && ram[0x770] == 2);
  int bot = bot_on();
  int stray = (idle && (ram[0x0E] != 8 || dead)) || (bot && (dead || lead));
  swap();
  if (dead && !idle && !bot) { swap(); fol_in = 0; luigi_leads ^= 1; died++; }
  else if (stray) {  // undone, and stepped 8 pixels back so the next fall clears the pole
    memcpy(ram, snap, sizeof ram);
    memcpy(fol, before, sizeof fol);
    int x = (*F(0x6D) << 8 | *F(0x86)) - 8;
    *F(0x86) = x & 0xFF; *F(0x6D) = x >> 8;
    if (dead) { respawn(0); *F(0x79E) = 8; }  // the computer's Luigi: back by Mario, blinking
  }
}

static int normal_play(void) {  // game mode, task "game engine", leader in control or growing/hurt
  u8 m = ram[0x0E];
  return ram[0x770] == 1 && ram[0x772] == 3 && (m == 8 || m == 9 || m == 10 || m == 12);
}

// Test aid: COOPSET="FRAME[-LAST]:ADDR:VAL,..." sets the follower's copy of a player variable
// before that frame's turn, or every frame of a range (the leader's own: rt.c --poke). test.sh
// uses it to place him, and to keep him flying alongside a flying leader.
static void coop_set(void) {
  const char *q = getenv("COOPSET");
  for (; q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : "") {
    int f, l; unsigned a, v;
    if (sscanf(q, "%d-%d:%x:%x", &f, &l, &a, &v) == 4) { if (frame_no >= f && frame_no <= l) *F(a) = v; }
    else if (sscanf(q, "%d:%x:%x", &f, &a, &v) == 3 && f == frame_no) *F(a) = v;
  }
}

// The computer's controller for Luigi (the follower; the game's bit order, A = $80): run to
// Mario, and on the ground jump a wall, a gap or an enemy ahead, or up to Mario when he is above.
// A jump holds A for 18 frames (a full jump), then lets go for a frame before the next.
// ponytail: looks one block ahead only, no route finding; a Luigi left 128 pixels behind for a
// second (the camera would hold Mario back) is put by Mario instead.
static u8 bot_pad(void) {
  static int far, hold, prev_a;
  if (away || !normal_play()) return 0;
  int lx = *F(0x6D) << 8 | *F(0x86), ly = *F(0xCE), dx = (ram[0x6D] << 8 | ram[0x86]) - lx;
  if (dx > 128 || dx < -128) { if (++far > 60) { to_leader(); far = 0; } } else far = 0;
  int dir = dx > 20 ? 1 : dx < -20 ? -1 : 0, jump = ram[0xCE] < ly - 20 && dx < 48 && dx > -48;
  u8 pad = dir > 0 ? 0x01 : dir < 0 ? 0x02 : 0;
  if (dx > 48 || dx < -48) pad |= 0x40;  // B: run
  if (dir) {
    int ax = lx + 8 + dir * 16, floor = 0;
    if (solid_at(ax, ly + 24) || (*F(0x754) == 0 && solid_at(ax, ly + 8))) jump = 1;  // a wall
    for (int y = ly + 32; y < 240; y += 16) floor |= solid_at(ax, y);
    if (!floor) jump = 1;  // a gap
    for (int i = 0; i < 5; i++) {  // Enemy_Flag, Enemy_PageLoc/X_Position, Enemy_Y_Position
      int ex = ((ram[0x6E + i] << 8 | ram[0x87 + i]) - lx) * dir, ey = ram[0xCF + i] - ly;
      if (ram[0x0F + i] && ex > 0 && ex < 56 && ey > -24 && ey < 32) jump = 1;
    }
  }
  if (hold) { hold--; pad |= 0x80; }
  else if (jump && *F(0x1D) == 0 && !prev_a) { hold = 18; pad |= 0x80; }  // Player_State: on the ground
  prev_a = pad & 0x80;
  return pad;
}

// $AEF0 in GameCoreRoutine, "sta SavedJoypadBits" with the controller bits in A: the leader is
// driven by his own controller, controller 2 when Luigi leads (the computer's, with --bot).
void coop_pads(void) {
  if (ram[0x770] == 0) luigi_leads = 0;  // title screen and its demo: the original game
  pad1 = ram[0x6FC];
  if (pad2_state) human2 = 1;
  if (bot_on()) ram[0x6FD] = bot_pad();
  if (coop_on && luigi_leads) A = ram[0x6FD];
}

// $8082, the start of every frame's interrupt routine (paused or not): once the game leaves the
// level (area change, lives screen, title), the follower is away until he reappears by the leader.
// Victory mode (after Bowser's axe) keeps him: he walks with the leader and is there for the thanks.
static int lives_screen;  // the "WORLD 1-1  x 3" screen is up: Mario on top, Luigi below
static void put_lives(u16 at, u8 lives) {  // "x N" from at, like the game's (crown from 10 lives)
  int n = lives + 1;
  pmwr(at + 2, 0x29);
  pmwr(at + 4, n >= 10 ? 0x9F : 0x24);
  pmwr(at + 5, n % 10);
}
void coop_frame(void) {
  if (ram[0x770] != 2 && (ram[0x772] != 3 || ram[0x770] != 1)) away = 1;
  // The lives screen, recognised by its "x" tile at $21CF (WorldLivesDisplay): Mario's line is
  // the game's own at $21CD, Luigi's the same three tile rows down, his icon a copy of the
  // game's moved 24 pixels down (rt.c colours the two as Mario and Luigi on this screen).
  lives_screen = coop_on && ram[0x770] == 1 && ram[0x772] != 3 && pmrd(0x21CF) == 0x29;
  if (!lives_screen) return;
  if (fresh_lives) { *F(0x75A) = ram[0x75A]; fresh_lives = 0; }  // a new game: the same lives as Mario
  u8 mario = luigi_leads ? *F(0x75A) : ram[0x75A], luigi = luigi_leads ? ram[0x75A] : *F(0x75A);
  put_lives(0x21CD, mario);
  put_lives(0x222D, luigi);
  luigi_nspr_next = 0;
  for (int i = 4; i < 256 && luigi_nspr_next < 16; i += 4)  // not sprite 0 (the status bar's)
    if (ram[0x200 + i] >= 0x40 && ram[0x200 + i] < 0x80) {
      u8 *o = luigi_oam_next + 4 * luigi_nspr_next++;
      memcpy(o, ram + 0x200 + i, 4);
      o[0] += 24;
    }
}

// $AF41 and $AF46 in GameEngine, which end the star music when the leader's star timer reaches 4
// (and run his flash): the music ends when the last brother's star does. Before the compare (1),
// a leader's 4 reads as 5 while the follower's star runs on; before the leader's timer is read
// (0), the follower's 4 ends the music when the leader's star is already below 4 (at 4 the game
// calls GetAreaMusic itself, above it his star plays on), and he is not below the screen.
void coop_star(int cmp) {
  if (!coop_on || away || fol_in) return;
  u8 f = *F(0x79F);
  if (cmp) { if (A == 4 && f > 4) A = 5; }
  else if (f == 4 && !ram[0x77F] && ram[0x79F] < 4 && *F(0xB5) < 2) call6502(0x90ED);
}

// ProcFireball_Bubble's throw, for the follower: the game's own runs for slot 0 only, later this
// frame, when his B press is already remembered as the previous frame's. Same rules: fire power,
// a fresh B press, the next of the two fireballs free, not crouching, climbing or off screen.
static u8 fol_fireball;  // a bit per fireball: thrown by the follower, not yet set off from him
static void fol_throw(void) {
  swap();
  int x = ram[0x6CE] & 1;  // FireballCounter: whose turn of the two
  if (ram[0x756] >= 2 && (ram[0x0A] & 0x40) && !(ram[0x0D] & 0x40) && !ram[0x24 + x] && ram[0xB5] == 1 &&
      !ram[0x714] && ram[0x1D] != 3) {
    ram[0xFF] = 0x20;                        // Square1SoundQueue: the fireball sound
    ram[0x24 + x] = 2;                       // Fireball_State: just thrown
    ram[0x711] = ram[0x70C];                 // FireballThrowingTimer, PlayerAnimTimer: his throwing pose
    ram[0x781] = ram[0x70C] - 1;
    ram[0x6CE]++;
    fol_fireball |= 1 << x;
  }
  swap();
}

// $B624 ProcFireball_Bubble: the game moves the fireballs only while the leader has fire power;
// with the follower's in the air and the leader without it, move them here (FireballObjCore).
void coop_fireballs(void) {
  u8 sx = X;
  if (coop_on && ram[0x756] < 2)
    for (X = 0; X < 2; X++) if (ram[0x24 + X]) call6502(0xB689);
  X = sx;
}

// $B697 in FireballObjCore, a fireball just thrown taking its place and direction from the player
// in slot 0: the follower's from him (in slot 0 up to $B6BE RunFB, where it starts flying).
static int fb_swapped;
void coop_fireball(int done) {
  if (!done && (fol_fireball & 1 << X)) { fol_fireball &= ~(1 << X); swap(); fb_swapped = 1; }
  else if (done && fb_swapped) { swap(); fb_swapped = 0; }
}

static u8 fol_pa;  // his A/B buttons of the frame before: the spring (enemy code, later) wants those
// $E14C in PlayerEnemyDiff, "sbc Player_PageLoc" (X = the enemy): the horizontal difference
// from "the player", which every enemy that aims asks for. Two of them may get the follower's:
// a piranha plant when he is nearer (it hides while the nearer brother is close), and Lakitu
// when he is the brother Lakitu picked. Which enemy asks is read from its JSR's return address.
// $00 (the low byte) is stored already, so it is redone; A and the carry are set so the game's
// own subtraction of the leader's page gives the follower's difference.
static int lakitu_luigi = -1;  // Lakitu's pick: 1 Luigi, 0 Mario, -1 none yet (pick at his next aim)
void coop_enemy_diff(void) {
  if (!coop_on || away || fol_in) return;
  u16 ret = ram[0x100 | (u8)(S + 1)] | ram[0x100 | (u8)(S + 2)] << 8;  // the JSR's address + 2
  int e = ram[0x6E + X] << 8 | ram[0x87 + X], fol;
  if (ret == 0xD3C3)  // MovePiranhaPlant
    fol = abs(e - (*F(0x6D) << 8 | *F(0x86))) < abs(e - (ram[0x6D] << 8 | ram[0x86]));
  else if (ret == 0xCF70) {  // PlayerLakituDiff
    if (lakitu_luigi < 0) lakitu_luigi = ram[0x7A7] & 1;  // PseudoRandomBitReg: replays stay exact
    fol = lakitu_luigi == !luigi_leads;  // the follower is Luigi unless Luigi leads
  } else return;
  if (!fol) return;
  ram[0] = ram[0x87 + X] - *F(0x86); C = ram[0x87 + X] >= *F(0x86);
  A += ram[0x6D] - *F(0x6D);
}

// $C3F1 in CreateSpiny, past its checks: Lakitu throws a Spiny, and picks a brother anew.
void coop_lakitu_pick(void) { lakitu_luigi = -1; }

// $AEF6, right after the leader's GameRoutines: the follower's turn.
void coop_turn(void) {
  coop_set();
  if (!coop_on) {
    if (!(normal_play() && (ram[0x6FD] & 0x10))) return;  // player 2 presses Start to join
    coop_on = 1; away = 1; fresh_lives = 1;
  }
  if (!normal_play()) {  // the leader's cutscene (pipe, flagpole): the follower lands and waits
    if (!away && ram[0x770] == 1 && ram[0x772] == 3 && *F(0x0E) == 8) run_as_follower(0xB0E9, 1);
    return;
  }
  if (away) { respawn(0); away = 0; }
  u8 mode = ram[0x770];
  u8 m = *F(0x0E);
  int d = died;
  run_as_follower(m == 8 ? 0xB0E9 : 0xB04A, 0);  // PlayerCtrlRoutine, else GameRoutines (grow/hurt/fire)
  // The screen's left edge holds him inside the screen (the game's ScrollHandler, KeepOnscr),
  // and walls don't stop it: as the leader scrolls on, it would push him into a pipe or wall
  // and through it. Once it has pushed him into one, he joins the leader instead.
  int left = ram[0x71A] << 8 | ram[0x71C];
  if (died == d && (*F(0x6D) << 8 | *F(0x86)) < left + 16 && stuck()) to_leader();
  fol_fireball = 0;
  if (died == d) fol_throw();  // (killed, he is in slot 0 now, the leader)
  // GameEngine's SaveAB step, which runs for slot 0 only: remember this frame's A/B buttons (a
  // jump keeps rising while A stays held) and clear the left/right buttons.
  fol_pa = *F(0x0D);
  *F(0x0D) = *F(0x0A); *F(0x0C) = 0;
  // Leader swap: he started a vine climb (1), side pipe (2), down pipe (3) or flagpole slide (4),
  // or touched Bowser's axe (the game switched to victory mode): he takes slot 0 and does it.
  m = *F(0x0E);
  if ((m >= 1 && m <= 4) || (mode == 1 && ram[0x770] == 2)) {
    swap(); fol_in = 0; luigi_leads ^= 1; fol_fireball = 0;
    // The status bar is rewritten only when an area loads, and victory mode loads none: write
    // the new leader's name straight onto the screen ($2043, where text 0 puts "MARIO").
    for (int i = 0; i < 5; i++) ppu_poke(0x2043 + i, NAMES[luigi_leads][i]);
  }
  if (getenv("COOPLOG"))  // debug aid: the follower's state after each of his turns
    fprintf(stderr, "f%d %s leads (lives %d); follower lives=%d x=%d y=%d yspd=%d state=%d mode=%02X ab=%02X prevab=%02X hurt=%d star=%d\n", frame_no,
            luigi_leads ? "luigi" : "mario", ram[0x75A], *F(0x75A), *F(0x6D) << 8 | *F(0x86), *F(0xCE), (int8_t)*F(0x9F), *F(0x1D),
            *F(0x0E), *F(0x0A), *F(0x0D), *F(0x79E), *F(0x79F));
}

// $82E9 in StartWorld1, as a new game starts from the title menu. "2 PLAYER GAME" starts the
// two-at-once game: the game itself runs as one player (no turns), Luigi joins from the start.
// "1 PLAYER GAME" clears any Luigi left from an earlier game (player 2 can still drop in).
void coop_new_game(void) {
  memset(fol, 0, sizeof fol);
  coop_on = ram[0x77A] == 1;  // NumberOfPlayers: 0 = one player, 1 = two
  luigi_leads = 0; away = 1; fresh_lives = 1; human2 = 0;
  lakitu_luigi = -1;
  *F(0x754) = 1; *F(0x756) = 0;  // small
  if (coop_on) ram[0x77A] = 0;
}

// $D853 PlayerEnemyCollision (X = the enemy) and $D7C4 PlayerHammerCollision (X = the hammer):
// check the follower against the same object first.
void coop_collide(u16 addr) {
  if (coop_on && !away && !fol_in && normal_play()) run_as_follower(addr, 0);
}

// $CE0B FirebarCollision, after the segment is drawn ($06/$07 = its screen X/Y): the follower is
// checked against it by the same routine, hurting or killing him by his own star and injury
// timers. The routine compares the OAM X of the player's first sprite ($0207), and his sprites
// are not in the game's OAM page: his Rel_XPos is the same value (his drawing set it), put there
// meanwhile. It also leaves $06 advanced by four for the next segment: that is for the game's
// own run, so it is put back.
void coop_firebar(void) {
  if (!(coop_on && !away && !fol_in && normal_play())) return;
  u8 x = ram[0x207], seg = ram[0x06];
  ram[0x207] = *F(0x3AD);
  run_as_follower(0xCE0B, 0);
  ram[0x207] = x; ram[0x06] = seg;
}

// Moving platforms: the game checks and carries the player in slot 0 only. Its collision flag
// (PlatformCollisionFlag, $03A2 + the platform's slot) says whom: large ones $FF none, else the
// platform he stands on; small ones 0 none, else which of its two halves.
static u8 fol_plat[6];   // the follower's flag, per platform
static u16 plat_x[6];    // the platform's position before it moved this frame

// $DB45 LargePlatformCollision and $DB7B SmallPlatformCollision (X = the platform): check the
// follower first, keeping his flag, and leave the leader's to the game.
void coop_platform(int small) {
  u8 x = X, lead = ram[0x3A2 + x];
  if (small && ram[0x747]) return;  // TimerControl: the game keeps the old flags too
  fol_plat[x] = small ? 0 : 0xFF;
  plat_x[x] = ram[0x6E + x] << 8 | ram[0x87 + x];  // Enemy_PageLoc, Enemy_X_Position
  if (!(coop_on && !away && !fol_in && normal_play())) return;
  run_as_follower(small ? 0xDB7B : 0xDB45, 0);
  fol_plat[x] = ram[0x3A2 + x];
  ram[0x3A2 + x] = lead;
}

// $C971 in RunLargePlatform, before the platform's own routine (it moves the platform, and
// carries whoever its flag names, in slot 0): when only the follower stands on it, he takes slot
// 0 for that routine, so the platform reacts to his weight and carries him, not the leader.
static int weighed;      // the follower is in slot 0 for this platform's routine
static u8 plat_scroll;   // Platform_X_Scroll: the leader's, the follower's ride doesn't scroll
void coop_weigh(void) {
  u8 x = X;
  if (ram[0x3A2 + x] != 0xFF || fol_plat[x] == 0xFF) return;
  swap();
  ram[0x3A2 + x] = fol_plat[x];
  plat_scroll = ram[0x3A1];
  weighed = 1;
}

// $E5C8 DrawLargePlatform and $D671 ChkSmallPlatCollision, after the platform moved: carry a
// follower standing on it along, and set him on it with the game's own routine.
void coop_ride(int small) {
  u8 x = X, f = fol_plat[x], sa = A;
  if (weighed && !small) {  // the routine carried him already: back out of slot 0
    swap();
    ram[0x3A2 + x] = 0xFF;
    ram[0x3A1] = plat_scroll;
    weighed = 0;
    return;
  }
  if (small ? !f : f == 0xFF) return;
  int px = (*F(0x6D) << 8 | *F(0x86)) + (ram[0x6E + x] << 8 | ram[0x87 + x]) - plat_x[x];
  *F(0x86) = px & 0xFF; *F(0x6D) = px >> 8;
  swap();
  if (small) A = f; else X = f;
  call6502(small ? 0xDC19 : 0xDC21);  // PositionPlayerOnS_Plat (A = half), PositionPlayerOnVPlat (X = platform)
  swap();
  A = sa; X = x;
}

// $91CD PlayerLoseLife, before the game takes the leader's life: the survivor's InjuryTimer and star
// end with the restart; if time ran out
// (GameTimerExpiredFlag), the follower loses one too, and his last one ends the game.
void coop_lose_life(void) {
  if (!coop_on) return;
  *F(0x79E) = *F(0x79F) = 0;  // any death restarts the level, which clears the timers (the leader's by the game)
  if (!ram[0x759]) return;
  if ((*F(0x75A))-- == 0) ram[0x75A] = 0;  // the game's own count then runs out: game over
}

// $AFC4 ScrollScreen, in the leader's turn, Y = how far the screen scrolls this frame: no further
// than keeps the follower on screen (8 pixels in from the left edge). The leader, no longer
// scrolling, is then held at the right edge by the game's own KeepOnscr. Not in cutscenes (the
// flagpole and victory walks scroll on their own).
// Test aid: COOPFREECAM=1 lets the camera follow the leader alone, as before this rule; the
// test.sh scenarios that fly Mario ahead to place things use it.
void coop_scroll(void) {
  static int free_cam = -1;
  if (free_cam < 0) free_cam = getenv("COOPFREECAM") != NULL;
  if (free_cam || !coop_on || away || fol_in || !normal_play()) return;
  int room = (*F(0x6D) << 8 | *F(0x86)) - 8 - (ram[0x71A] << 8 | ram[0x71C]);  // ScreenLeft
  if (room < Y) Y = room < 0 ? 0 : room;
}

// $B8BD in JumpspringHandler (after its first call) and $B90B (where it has drawn the spring):
// the spring nudges and launches the player in slot 0, and holding A (a fresh press) makes the
// high bounce. JumpspringAnimCtrl and JumpspringForce are each brother's own (he set them
// landing); when only the follower's is running, he takes slot 0 for the handler.
// If both stand on springs, the leader's goes first.
static int sprung;       // the follower is in slot 0 for this spring's handler
void coop_spring(int done) {
  static u8 pa;
  if (!done && coop_on && !away && !fol_in && normal_play() && !ram[0x70E] && *F(0x70E)) {
    swap();
    pa = ram[0x0D]; ram[0x0D] = fol_pa;
    sprung = 1;
  } else if (done && sprung) {
    if (ram[0x70E] && !ram[0x786]) { ram[0x786] = 4; ram[0x70E]++; }  // the handler's last step, for his spring
    ram[0x0D] = pa;
    swap();
    sprung = 0;
  }
}

// $C08C, ExecGameLoopback (a castle maze sends the leader back four pages): the follower too.
void coop_loopback(void) {
  if (coop_on && !away) *F(0x6D) -= 4;
}

// $B7B8 ProcessWhirlpools, before the leader's: the follower is pulled by the game's own routine.
void coop_whirlpool(void) {
  if (coop_on && !away && !fol_in && normal_play()) run_as_follower(0xB7B8, 0);
}

// $8100, DecTimers: the follower's timers tick by the same rule as the game's. Frame timers
// ($0780-$0794) every frame; interval timers when IntervalTimerControl ($077F) is about to expire.
void coop_timers(void) {
  if (!coop_on) return;
  int interval = ram[0x77F] == 0;
  for (int i = 0; i < NSLICE; i++)
    if (SLICE[i] >= 0x780 && SLICE[i] <= 0x7A3 && fol[i] && (SLICE[i] <= 0x794 || interval)) fol[i]--;
}

// $83B0 SetupVictoryMode (setup: the bridge is gone) and $83BD PlayerVictoryWalk (the leader
// walks to Toad). The follower may have been on the bridge, or behind the gap: he is put where
// the leader stands, then walks right while he does, up to 16 pixels behind him, with the
// game's own AutoControlPlayer (A = the buttons). As in the leader's other cutscenes, a turn
// that would kill him is undone.
void coop_victory_walk(int setup) {
  if (!coop_on || away) return;
  if (setup) { respawn(0); *F(0x86) = ram[0x86]; *F(0x6D) = ram[0x6D]; return; }
  u8 sa = A;
  int walk = ram[0x6D] != ram[0x34] || ram[0x86] < 0x60;  // the game's rule for the leader
  A = walk && (*F(0x6D) << 8 | *F(0x86)) + 16 < (ram[0x6D] << 8 | ram[0x86]);  // 1 = Right
  run_as_follower(0xB0E6, 1);
  A = sa;
}

// $8839 in WriteGameText, X = the text just written to VRAM_Buffer1: the status bar ("MARIO",
// text 0) names the leader, as the original names the current player. The game runs as one
// player (CurrentPlayer stays 0: the shared score, his palette), so only the name is changed.
void coop_name(void) {
  if (coop_on && luigi_leads && X == 0 && ram[0x770]) memcpy(ram + 0x304, NAMES[1], 5);
}

// $843C PrintMsg in PrintVictoryMessages, Y = the message (0 "THANK YOU MARIO!", 1 LUIGI): thank
// the leader, who touched the axe.
void coop_thanks(void) {
  if (coop_on && luigi_leads && Y == 0) Y = 1;
}

// $AF1C after the leader is drawn in GameEngine, and $838E VictoryMode (each of its tasks): draw
// the follower with the game's own routine, then move the sprites it wrote out of the game's OAM
// page into the extra slots (shown after the next OAM DMA, which then empties them for the next frame).
void coop_draw(void) {
  if (!coop_on || away || fol_in) return;
  static u8 page[256];
  memcpy(page, ram + 0x200, 256);
  for (int i = 0; i < 256; i += 4) ram[0x200 + i] = 0xF8;  // all hidden: what shows is his
  if (*F(0xB5) < 2) {  // his star flash, by the game's rule (CyclePlayerPalette, ResetPalStar)
    u8 f = *F(0x79F), c = f ? ram[0x09] >> (f >= 8 ? 1 : 3) & 3 : 0;
    *F(0x3C4) = *F(0x3C4) & ~3 | c;
  }
  swap();
  call6502(0xF180); call6502(0xF12A); call6502(0xEEE9);  // GetPlayerOffscreenBits, RelativePlayerPosition, PlayerGfxHandler
  swap();
  luigi_nspr_next = 0;
  for (int i = 0; i < 256 && luigi_nspr_next < 16; i += 4)
    if (ram[0x200 + i] < 0xEF) memcpy(luigi_oam_next + 4 * luigi_nspr_next++, ram + 0x200 + i, 4);
  memcpy(ram + 0x200, page, 256);
}

// Colours, from the game's PlayerColors table: [luigi][fiery], entries 1-3 of the sprite palette.
// The game's fiery Luigi is fiery Mario (never both on screen there); here his red is his green.
static const u8 COLORS[2][2][4] = {{{0x22, 0x16, 0x27, 0x18}, {0x22, 0x37, 0x27, 0x16}},
                                   {{0x22, 0x30, 0x27, 0x19}, {0x22, 0x37, 0x27, 0x19}}};
// For rt.c: the follower's colours, and the leader's (the game's sprite palette 0 in two-player
// mode, so the leader shows as himself, whoever he is, and the follower's fire flower, which
// rewrites that palette, recolours nobody).
const u8 *follower_colors(void) { return lives_screen ? COLORS[1][0] : COLORS[!luigi_leads][*F(0x756) == 2]; }
const u8 *leader_colors(void) {
  return !coop_on ? NULL : lives_screen ? COLORS[0][0] : COLORS[luigi_leads][ram[0x756] == 2];
}
