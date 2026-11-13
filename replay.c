// Recording and replay: everything a run depends on besides the ROM, so that it plays back frame
// for frame. Included by rt.c, after save.c. The game is deterministic: from power-on, the same
// controllers on the same frames give the same run. A recording is text, from power-on:
//   build e8274cd             the commit built from (-dirty: plus uncommitted changes)
//   bot 1                     the computer plays Luigi (coop.c)
//   save 3 1 2 6              the save at power-on (save.c: world level area half, from 0)
//   120 keys 08 00            from frame 120, controllers 1 and 2 (rt.c's bits: A B Sel Start U D L R)
//   0 cheats lives=1 world=4  the cheats from that frame (cheats.c; Android rereads them on return)
//   # 1532 game               a game began (a comment, for --start)
// Android records every launch to files/replays/ (`make replays` copies them here); the Mac
// with --record FILE. --replay FILE plays one back. A replay needs the build that recorded it
// (the code is part of the run): one from another build plays on, with a warning naming the
// commit to build it from. ponytail: old recordings are never deleted (a few KB an hour).
#ifndef BUILD
#define BUILD "unknown"
#endif
static FILE *rec;
static int rec_p1 = -1, rec_p2 = -1;  // the controllers as last written
static int last_mode;                 // OperMode last frame: 0 -> 1 is a game starting
static char cheats_now[256];          // the cheats in force (cheats_parse), for the recording's start

static FILE *rp;          // the recording being replayed
static int rp_p1, rp_p2;  // its controllers now
static char rp_line[300];
static int rp_frame = -1; // the frame of the event in rp_line (-1: none read yet)

void rec_cheats(const char *s) {
  snprintf(cheats_now, sizeof cheats_now, "%s", s);
  cheats_now[strcspn(cheats_now, "\r\n")] = 0;
  if (rec) fprintf(rec, "%d cheats %s\n", frame_no, cheats_now);
}

void rec_start(const char *path) {
  if (!(rec = fopen(path, "w"))) return;
  setvbuf(rec, NULL, _IOLBF, 0);  // a line at a time: a killed app keeps all but the last frame
  fprintf(rec, "build %s\nbot %d\n", BUILD, coop_bot);
  if (saved_w >= 0) fprintf(rec, "save %d %d %d %d\n", saved_w, saved_l, saved_a, saved_half);
  fprintf(rec, "%d cheats %s\n", frame_no, cheats_now);
}

static void rp_next(void) {  // read up to the next event, applying the header lines on the way
  for (rp_frame = -1; rp_frame < 0 && fgets(rp_line, sizeof rp_line, rp);) {
    int w, l, a, h;
    char b[64];
    if (sscanf(rp_line, "build %63s", b) == 1) {
      if (strcmp(b, BUILD)) fprintf(stderr, "warning: recorded by build %s, this is %s: the replay may go its own way\n", b, BUILD);
      continue;
    }
    if (sscanf(rp_line, "bot %d", &coop_bot) == 1) continue;
    if (sscanf(rp_line, "save %d %d %d %d", &w, &l, &a, &h) == 4) { saved_w = w, saved_l = l, saved_a = a, saved_half = h; continue; }
    if (rp_line[0] != '#') sscanf(rp_line, "%d", &rp_frame);
  }
}

// Returns an error, or NULL. Before the first frame.
const char *replay_open(const char *path) {
  if (!(rp = fopen(path, "r"))) return "can't read the replay";
  save_path = NULL;  // a replay never writes the save
  rp_next();
  return NULL;
}

// At the start of every frame (frame()): the replay's controllers and cheats for this frame,
// or this frame's controllers written to the recording.
static void rec_frame(void) {
  for (; rp && rp_frame >= 0 && rp_frame <= frame_no; rp_next()) {
    unsigned p1, p2;
    char *c = strstr(rp_line, " cheats ");
    if (sscanf(rp_line, "%*d keys %x %x", &p1, &p2) == 2) rp_p1 = p1, rp_p2 = p2;
    else if (c) cheats_parse(c + 8);
  }
  if (rp) pad_state = rp_p1, pad2_state = rp_p2;
  if (!rec) return;
  if (ram[0x770] == 1 && last_mode == 0) fprintf(rec, "# %d game\n", frame_no);
  last_mode = ram[0x770];
  if (pad_state == rec_p1 && pad2_state == rec_p2) return;
  rec_p1 = pad_state, rec_p2 = pad2_state;
  fprintf(rec, "%d keys %02x %02x\n", frame_no, pad_state, pad2_state);
}
