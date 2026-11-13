// Playground: a test level for two players, our own layout (no data from the game), played in
// place of World 1-1: press Up on the title screen (the status bar then reads PLAYGROUND; Down
// takes it back), then start a game as usual. Included by rt.c.
// The game reads a level's objects and enemies through two pointers that GetAreaDataAddrs sets
// from the area number; the mod there points them at these bytes, which rt.c serves at $6000
// (cartridge RAM on other boards, unused by this game). Level data format: doppelganger's SMB
// disassembly (ref/smbdis.asm, DecodeAreaData and ProcessEnemyData).
//
// Since it stands in for 1-1, the game's own 1-1 links still work: the pipe on page 1 leads to
// 1-1's coin room, whose exit comes back out of the pipe on page 10, and the flagpole goes on
// to 1-2. Pages (256 pixels each):
//   0  blocks: ? power-up, brick of coins, ? coin, brick with a star, brick with a 1-up; a spring
//   1  pipe to the coin room (press Down on it)
//   2  sideways platform          3  falling platform, hidden 1-up block above it
//   4  balance platforms (pulley) 5  platform that moves right once stood on
//   6  up-down platform, two lifts   7  big lift, Goombas, a Koopa
//   8  staircase   10  return pipe from the coin room   11-12  staircase, flagpole, castle
static const u8 PLAYGROUND[0x200] = {
  // level objects: header (400 time, start on foot; hills, plain floor), then two bytes each:
  // column << 4 | row, then $80 = on the next page | kind << 4 | size
  0x50, 0x21,
  0x57, 0x00, 0x67, 0x07, 0x77, 0x01, 0x87, 0x06, 0x97, 0x08,  // page 0: the block row
  0xD9, 0x0B,                                                  //   spring
  0x48, 0xFA, 0xA9, 0x71,                                      // 1: pipe to the coin room, small pipe
  0x45, 0xC3,                                                  // 2: coins
  0x87, 0x83,                                                  // 3: hidden 1-up block
  0x2C, 0x97, 0x2F, 0x12, 0x9F, 0x14,                          // 4: pulley rope, its two ropes
  0x86, 0xC2,                                                  // 5: coins
  0x14, 0xC1,                                                  // 6: coins
  0x47, 0xA0,                                                  // 7: a brick
  0x2F, 0xB4,                                                  // 8: staircase
  0x46, 0xC2,                                                  // 9: coins
  0x39, 0xF1,                                                  // 10: the coin room's return pipe
  0x5F, 0xB8,                                                  // 11: staircase
  0x6D, 0xC1, 0xAF, 0x26,                                      // 12: flagpole, castle
  0xFD,
  // enemies and platforms, from $6100: column << 4 | row, then the object ($0F: go to page N)
  [0x100] =
  0x1E, 0xC2, 0x00,              // pipes on this level lead to area $C2 (1-1's coin room), page 0
  0x0F, 0x02, 0x68, 0x28,        // page 2: sideways platform
  0x0F, 0x03, 0x37, 0x29,        // 3: falling platform
  0x0F, 0x04, 0x16, 0x24, 0x88, 0x24,  // 4: the two balance platforms
  0x0F, 0x05, 0x29, 0x2A,        // 5: moves right when stood on
  0x0F, 0x06, 0x46, 0x25, 0x9A, 0x2B, 0xCA, 0x2C,  // 6: up-down platform, lift up, lift down
  0x0F, 0x07, 0x26, 0x26, 0x8B, 0x06, 0xAB, 0x06, 0xCB, 0x00,  // 7: big lift, two Goombas, a Koopa
  0xFF,
};
static int playground;  // this game plays the playground in place of 1-1

u8 playground_rd(u16 a) { return a < 0x6200 ? PLAYGROUND[a - 0x6000] : 0; }

// $8082, every frame: on the title screen, Up arms the playground and Down disarms it, shown in
// the status bar under WORLD (its tiles saved and put back). Back on the title after a game, it
// is off again.
void playground_frame(void) {
  static const u8 TEXT[10] = {0x19, 0x15, 0x0A, 0x22, 0x10, 0x1B, 0x18, 0x1E, 0x17, 0x0D};  // PLAYGROUND
  static u8 saved[10], shown, last_mode;
  u8 mode = ram[0x770];
  if (mode == 0 && last_mode != 0) playground = 0;
  last_mode = mode;
  if (mode != 0) return;
  u8 pad = pad_state | pad2_state;  // the controllers as held (the title menu clears its own copy)
  if (pad & 0x10) playground = 1;    // Up
  if (pad & 0x20) playground = 0;    // Down
  if (playground == shown) return;
  for (int i = 0; i < 10; i++) {
    if (playground) saved[i] = pmrd(0x2072 + i);
    pmwr(0x2072 + i, playground ? TEXT[i] : saved[i]);
  }
  shown = playground;
}

// $9C58 in GetAreaDataAddrs, the level and enemy pointers just loaded: 1-1 (area $25; $A5 is
// the same area entered from its coin room) becomes the playground, outside the title's demo.
void playground_area(void) {
  if (!playground || !ram[0x770] || (ram[0x750] & 0x7F) != 0x25) return;
  ram[0xE7] = 0x00; ram[0xE8] = 0x60;  // AreaData
  ram[0xE9] = 0x00; ram[0xEA] = 0x61;  // EnemyData
}
