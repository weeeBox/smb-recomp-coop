// Saved progress: the level reached and whether its checkpoint (the halfway point the game itself
// restarts from after a death) was passed. Included by rt.c, after cheats.c.
// A save is written whenever play reaches a new level or passes a checkpoint; on the title
// screen the status bar's world-level then shows the save, and starting a game (one or two
// players) begins there, at the checkpoint if it was passed, with a new game's lives and score.
// Left on the title screen starts from 1-1 instead (or the cheats' level), Right takes the save
// back. The playground doesn't save or resume. The file is files/save.txt on Android; on the Mac
// only with --save FILE (so the tests never see one).
static const char *save_path;
static int saved_w = -1, saved_l, saved_a, saved_half;  // saved_w < 0: no save
static int save_armed = 1;                               // the next new game resumes the save

void save_load(const char *path) {
  save_path = path;
  FILE *f = fopen(path, "r");
  if (!f) return;
  int w, l, a, h;
  if (fscanf(f, "world=%d level=%d area=%d half=%d", &w, &l, &a, &h) == 4 && w >= 0 && w < 8 && l >= 0 && l < 4 && a >= 0 && a < 8 && h >= 0 && h < 16)
    saved_w = w, saved_l = l, saved_a = a, saved_half = h;
  fclose(f);
}

static void save_write(void) {
  if (!save_path) return;
  char tmp[600];
  snprintf(tmp, sizeof tmp, "%s.tmp", save_path);  // a crash mid-write leaves the old save whole
  FILE *f = fopen(tmp, "w");
  if (!f) return;
  fprintf(f, "world=%d level=%d area=%d half=%d\n", saved_w, saved_l, saved_a, saved_half);
  if (fclose(f) == 0) rename(tmp, save_path);
}

// HalfwayPageNybbles ($91BD), read as PlayerLoseLife reads it: two bytes per world, a nybble per
// level (high for levels 1 and 3). 0: the level has no checkpoint.
static int halfway_page(int w, int l) {
  u8 b = rd(0x91BD + w * 2 + (l >> 1 & 1));
  return l & 1 ? b & 15 : b >> 4;
}

// $8082, every frame (after playground_frame).
void save_frame(void) {
  u8 mode = ram[0x770], task = ram[0x772];  // OperMode, OperMode_Task
  if (mode == 0) {  // the title screen (and its demo): Left/Right, and the save shown as WORLD w-l
    if (saved_w < 0 || playground) return;  // the playground's name covers WORLD's digits
    u8 pad = pad_state | pad2_state;
    if (pad & 0x40) save_armed = 0;
    if (pad & 0x80) save_armed = 1;
    pmwr(0x2073, save_armed ? saved_w + 1 : ram[0x75F] + 1);
    pmwr(0x2075, save_armed ? saved_l + 1 : ram[0x75C] + 1);
    return;
  }
  // Only while the game core runs (OperMode_Task 3): the end of a level counts up the level and
  // loads the next area together, setting the task back to 0.
  if (mode != 1 || task != 3 || playground) return;
  u8 w = ram[0x75F], l = ram[0x75C], a = ram[0x760];  // WorldNumber, LevelNumber, AreaNumber
  int p = halfway_page(w, l), half = p && p <= ram[0x71A] ? p : 0;  // ScreenLeft_PageLoc
  int same = w == saved_w && l == saved_l && a == saved_a;
  if (same && half <= saved_half) return;  // a coin room (page 0) doesn't take the checkpoint back
  saved_w = w, saved_l = l, saved_a = a, saved_half = half;
  save_write();
}

// $82E9 in StartWorld1, a new game, after cheats_new_game: begin at the save. HalfwayPage is
// what InitializeArea starts the area from, as after a death past the checkpoint.
void save_new_game(void) {
  if (saved_w < 0 || !save_armed || playground) return;
  ram[0x75F] = saved_w;
  ram[0x75C] = saved_l;
  ram[0x760] = saved_a;
  ram[0x75B] = saved_half;  // HalfwayPage
  call6502(0x9C03);         // LoadAreaPointer
}
