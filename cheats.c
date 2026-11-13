// Cheats, set in the Android app's settings screen (android/Cheats.java, reached from the
// system's App info page) or with --cheats on the Mac. Included by rt.c, after coop.c.
// Settings are "key=value" words: lives=1 world=4 level=2 time=1 fire=1 (world and level
// count from 1). Addresses are from doppelganger's SMB disassembly (tools/smbsyms.py).
static int cheat_lives, cheat_world, cheat_level, cheat_time, cheat_fire;  // world, level: from 0, as the game counts

void cheats_parse(const char *s) {
  struct { const char *key; int *v; } K[] = {{"lives=", &cheat_lives}, {"world=", &cheat_world},
      {"level=", &cheat_level}, {"time=", &cheat_time}, {"fire=", &cheat_fire}};
  for (int i = 0; i < 5; i++) { const char *p = strstr(s, K[i].key); *K[i].v = p ? atoi(p + strlen(K[i].key)) : 0; }
  cheat_world = cheat_world >= 1 && cheat_world <= 8 ? cheat_world - 1 : 0;
  cheat_level = cheat_level >= 1 && cheat_level <= 4 ? cheat_level - 1 : 0;
  rec_cheats(s);  // replay.c
}

// $82E9 in StartWorld1, a new game (after the area pointer for 1-1, or the continue world, was
// loaded): start at the chosen level. Worlds 1, 2, 4 and 7 have an extra area before level 2
// (the walk into the pipe, area 1), so levels 3 and 4 are areas 3 and 4.
void cheats_new_game(void) {
  if (!cheat_world && !cheat_level) return;
  int w = cheat_world, l = cheat_level;
  ram[0x75F] = w;                                                     // WorldNumber
  ram[0x75C] = l;                                                     // LevelNumber
  ram[0x760] = l + (l >= 2 && (w == 0 || w == 1 || w == 3 || w == 6));  // AreaNumber
  call6502(0x9C03);                                                   // LoadAreaPointer
}

// $8082, every frame.
void cheats_frame(void) {
  if (ram[0x770] != 1) return;  // OperMode: only while playing (not the title's demo)
  if (cheat_lives) {            // NumberofLives never drops below 2 (the screen shows x 3)
    if (ram[0x75A] < 2) ram[0x75A] = 2;
    if (*F(0x75A) < 2) *F(0x75A) = 2;  // Luigi's (coop.c)
  }
  if (cheat_time) ram[0x787] = 0x18;  // GameTimerCtrlTimer: the clock never ticks down
  // Fire power at the start of every life and area: before the level runs (OperMode_Task 3).
  if (cheat_fire && ram[0x772] != 3) {
    ram[0x754] = 0; ram[0x756] = 2;    // PlayerSize big, PlayerStatus fiery
    *F(0x754) = 0; *F(0x756) = 2;
  }
}
