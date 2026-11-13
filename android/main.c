// Android frontend (NativeActivity; the only Java is the cheats screen, Cheats.java): OpenGL ES draws the frame at integer scale,
// AAudio for sound, the device's gamepad buttons (and any USB/Bluetooth pad) for input.
// The emulated hardware and the run loop are rt.c, included here as a library.
// The ROM is read from the app's private files folder (android/install.sh copies it there);
// without it, or with the wrong file, the screen stays red and logcat says why.
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <aaudio/AAudio.h>
#include <android/log.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <math.h>
#include <stdatomic.h>
#include <sys/stat.h>
#include <time.h>
#define RT_LIB
#define MISSES "/dev/null"
#define ROM_DEFAULT ""
#include "rt.c"

#define ALOG(...) __android_log_print(ANDROID_LOG_WARN, "recomp", __VA_ARGS__)

static int ready, focused, redraw = 1;  // redraw: the window is new, present even an unchanged frame
static u8 keys[2], hat[2];  // per player: gamepad buttons, and d-pad bits from the hat/stick axes
static u8 latch[2];         // buttons pressed since the last frame: a tap shorter than a frame still counts
// Player 1 is the first gamepad pressed after launch (normally the device's own buttons);
// any other controller is player 2, who joins as Luigi with Start (coop.c).
static int p1_dev = -2;
static int player(AInputEvent *ev) {
  // Only a gamepad can claim a player: keys from anything else (adb, a keyboard) are player 1's.
  if (!(AInputEvent_getSource(ev) & (AINPUT_SOURCE_GAMEPAD | AINPUT_SOURCE_JOYSTICK))) return 0;
  int id = AInputEvent_getDeviceId(ev);
  if (p1_dev == -2) p1_dev = id;
  return id != p1_dev;
}
static u8 back_bit;   // the game button the held Back key stands for (0: Back went to the system)
static long nkey, npad;  // key events seen, frames run with a button held: in stats.txt

// Back steps back through the game rather than leaving it. $0770 is the game's mode:
// 0 on the title screen (and its demo), 1 playing, 2 the ending, 3 game over.
#define OPER_MODE 0x770
static u8 back_button(void) { return ram[OPER_MODE] == 0 ? 0 : 0x08; }  // title: leave; else Start (pause)

// Sound: rt.c's APU leaves each frame's samples (mono, APU_HZ) in abuf; they queue here.
#define RB 16384
static int16_t ring[RB];
static atomic_int r_head, r_tail;  // head: written by the game loop, tail: by the audio thread
static int queued(void) { return (atomic_load(&r_head) - atomic_load(&r_tail)) & (RB - 1); }
static void queue_frame_audio(void) {
  int h = atomic_load(&r_head);
  for (int i = 0; i < an && queued() < RB - 1; i++) { ring[h] = abuf[i]; h = (h + 1) & (RB - 1); atomic_store(&r_head, h); }
  an = 0;
}
static AAudioStream *stream;
static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *ud, void *data, int32_t n) {
  int16_t *o = data;
  int t = atomic_load(&r_tail), h = atomic_load(&r_head), i = 0;
  for (; i < n && t != h; i++, t = (t + 1) & (RB - 1)) o[i] = ring[t];
  atomic_store(&r_tail, t);
  for (; i < n; i++) o[i] = 0;
  return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static char stats_path[512];
static void audio_start(void) {
  AAudioStreamBuilder *b;
  if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
  AAudioStreamBuilder_setSampleRate(b, APU_HZ);
  AAudioStreamBuilder_setChannelCount(b, 1);
  AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
#ifndef AUDIO_MODE
#define AUDIO_MODE AAUDIO_PERFORMANCE_MODE_LOW_LATENCY
#endif
  AAudioStreamBuilder_setPerformanceMode(b, AUDIO_MODE);
  AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
  if (AAudioStreamBuilder_openStream(b, &stream) != AAUDIO_OK) stream = NULL;
  AAudioStreamBuilder_delete(b);
  if (stream) AAudioStream_requestStart(stream);
  ALOG("audio %s", stream ? "on" : "unavailable");
}

// NES pad bits: 0 A, 1 B, 2 Select, 3 Start, 4 Up, 5 Down, 6 Left, 7 Right
static u8 key_bit(int code) {
  switch (code) {
  case AKEYCODE_DPAD_RIGHT: return 0x80;
  case AKEYCODE_DPAD_LEFT: return 0x40;
  case AKEYCODE_DPAD_UP: return 0x10;
  case AKEYCODE_DPAD_DOWN: return 0x20;
  case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_B: return 0x01;  // NES A: jump (Xbox layout: bottom, right)
  case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_Y: return 0x02;  // NES B: run / fire (left, top)
  case AKEYCODE_BUTTON_SELECT: case AKEYCODE_BUTTON_L1: return 0x04;
  case AKEYCODE_BUTTON_START: case AKEYCODE_BUTTON_R1: return 0x08;
  }
  return 0;
}

static int32_t on_input(struct android_app *app, AInputEvent *ev) {
  if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
    int code = AKeyEvent_getKeyCode(ev), act = AKeyEvent_getAction(ev);
    if (code == AKEYCODE_BACK) {
      if (act == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(ev) == 0) back_bit = ready ? back_button() : 0;
      if (!back_bit) return 0;  // the system handles it: the activity closes
      if (act == AKEY_EVENT_ACTION_DOWN) { keys[0] |= back_bit; latch[0] |= back_bit; }
      else if (act == AKEY_EVENT_ACTION_UP) { keys[0] &= ~back_bit; back_bit = 0; }
      return 1;
    }
    u8 b = key_bit(code); nkey++;
    if (!b) return 0;  // volume etc. stay with the system
    int p = player(ev);
    // A diagnostic for a run button found dead after locking the device: every press and
    // release (not repeats) and the player it went to, in logcat (adb logcat -s recomp).
    if (!AKeyEvent_getRepeatCount(ev))
      ALOG("key %d %s dev %d -> player %d flags %x", code, act == AKEY_EVENT_ACTION_DOWN ? "down" : "up",
           AInputEvent_getDeviceId(ev), p + 1, AKeyEvent_getFlags(ev));
    if (act == AKEY_EVENT_ACTION_DOWN) { keys[p] |= b; latch[p] |= b; }
    else if (act == AKEY_EVENT_ACTION_UP) keys[p] &= ~b;
    return 1;
  }
  if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION &&
      (AInputEvent_getSource(ev) & (AINPUT_SOURCE_JOYSTICK | AINPUT_SOURCE_GAMEPAD))) {
    float x = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0) + AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0);
    float y = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0) + AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
    hat[player(ev)] = (x > 0.5f) << 7 | (x < -0.5f) << 6 | (y < -0.5f) << 4 | (y > 0.5f) << 5;
    return 1;
  }
  return 0;
}

static void gl_init(ANativeWindow *win);
static void gl_term(void);
// The cheats screen (Cheats.java) leaves its choices in files/cheats.txt; read on every return.
static void load_cheats(struct android_app *app) {
  char path[512], s[256] = "";
  snprintf(path, sizeof path, "%s/cheats.txt", app->activity->internalDataPath);
  FILE *f = fopen(path, "r");
  if (f) { fgets(s, sizeof s, f); fclose(f); }
  cheats_parse(s);
  ALOG("cheats: %s", f ? s : "none");
}
static void on_cmd(struct android_app *app, int32_t cmd) {
  switch (cmd) {
  case APP_CMD_INIT_WINDOW:
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON, 0);  // no sleeping mid-game
    gl_init(app->window); redraw = 1; break;
  case APP_CMD_TERM_WINDOW: gl_term(); break;
  case APP_CMD_GAINED_FOCUS: focused = 1; ALOG("focus gained (player 1 = device %d)", p1_dev); load_cheats(app); break;
  case APP_CMD_LOST_FOCUS: ALOG("focus lost"); focused = 0; memset(keys, 0, 2); memset(hat, 0, 2); memset(latch, 0, 2); back_bit = 0; break;
  }
}

// GPU path: the frame is a 256x240 texture drawn at the largest integer scale, unfiltered.
static EGLDisplay dpy = EGL_NO_DISPLAY;
static EGLSurface surf = EGL_NO_SURFACE;
static EGLContext ctx = EGL_NO_CONTEXT;
static GLuint tex;

static GLuint shader(GLenum type, const char *src) {
  GLuint s = glCreateShader(type);
  glShaderSource(s, 1, &src, NULL);
  glCompileShader(s);
  return s;
}

static void gl_init(ANativeWindow *win) {
  dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
  eglInitialize(dpy, NULL, NULL);
  const EGLint ca[] = {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                       EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE};
  EGLConfig cfg; EGLint n;
  eglChooseConfig(dpy, ca, &cfg, 1, &n);
  surf = eglCreateWindowSurface(dpy, cfg, win, NULL);
  const EGLint xa[] = {EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE};
  ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
  eglMakeCurrent(dpy, surf, surf, ctx);
  eglSwapInterval(dpy, 1);
  GLuint p = glCreateProgram();
  glAttachShader(p, shader(GL_VERTEX_SHADER,
    "attribute vec2 v; varying vec2 t; void main(){ t = vec2(v.x, -v.y) * 0.5 + 0.5; gl_Position = vec4(v, 0.0, 1.0); }"));
  // rt.c's pixels are 0x00RRGGBB, i.e. bytes B,G,R in memory: swap back in the shader
  glAttachShader(p, shader(GL_FRAGMENT_SHADER,
    "precision mediump float; varying vec2 t; uniform sampler2D s; void main(){ gl_FragColor = vec4(texture2D(s, t).bgr, 1.0); }"));
  glBindAttribLocation(p, 0, "v");
  glLinkProgram(p);
  glUseProgram(p);
  static const float quad[] = {-1, -1, 1, -1, -1, 1, 1, 1};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
  glEnableVertexAttribArray(0);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 256, 240, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
}

static void gl_term(void) {
  if (dpy == EGL_NO_DISPLAY) return;
  eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext(dpy, ctx);
  eglDestroySurface(dpy, surf);
  eglTerminate(dpy);
  dpy = EGL_NO_DISPLAY;
}

static void draw(void) {
  if (dpy == EGL_NO_DISPLAY) return;
  EGLint w, h;
  eglQuerySurface(dpy, surf, EGL_WIDTH, &w);
  eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
  int s = w / 256 < h / 240 ? w / 256 : h / 240;
  if (s < 1) s = 1;
  glClearColor(ready ? 0 : 0.75f, 0, 0, 1);  // red: no ROM, see logcat
  glClear(GL_COLOR_BUFFER_BIT);
  if (ready) {
    glViewport((w - 256 * s) / 2, (h - 240 * s) / 2, 256 * s, 240 * s);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 240, GL_RGBA, GL_UNSIGNED_BYTE, fb);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  }
  eglSwapBuffers(dpy, surf);  // waits for the display: about one refresh
}

// The emulator is process-wide state. If Android recreates the activity in the same process,
// the new one carries on with the running game; loading again would reset half the machine.
static void load(struct android_app *app) {
  if (ready) return;
  char path[512];
  snprintf(path, sizeof path, "%s/" NAME ".nes", app->activity->internalDataPath);
  snprintf(stats_path, sizeof stats_path, "%s/stats.txt", app->activity->internalDataPath);
  const char *err = load_rom(path);
  if (err) { ALOG("%s: %s", path, err); return; }
  boot();
  static char save[512];  // progress (save.c): kept for the life of the process
  snprintf(save, sizeof save, "%s/save.txt", app->activity->internalDataPath);
  save_load(save);
  coop_bot = 1;  // 2 PLAYER GAME without a second controller: the computer plays Luigi (coop.c)
  // Every launch is recorded (replay.c) to files/replays/YYYYMMDD-HHMMSS.txt.
  static char rec_path[600];
  time_t now = time(NULL);
  snprintf(rec_path, sizeof rec_path, "%s/replays", app->activity->internalDataPath);
  mkdir(rec_path, 0700);
  strftime(rec_path + strlen(rec_path), 32, "/%Y%m%d-%H%M%S.txt", localtime(&now));
  rec_start(rec_path);
  ALOG("recording to %s", rec_path);
  ready = 1;
}

static double now_s(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec * 1e-9;
}

// Full screen: hide the navigation bar (the home handle) and the status bar; a swipe from the edge
// shows them for a moment (immersive sticky). The call must come from the UI thread, so it runs
// in the window-focus callback, wrapped around the native glue's own, which Android calls there
// on every focus gain (the bars come back while the app is away).
static void (*glue_on_focus)(ANativeActivity *, int);
static void on_focus_ui(ANativeActivity *a, int has_focus) {
  if (has_focus) {
    JNIEnv *env = a->env;
    jobject win = (*env)->CallObjectMethod(env, a->clazz,
        (*env)->GetMethodID(env, (*env)->GetObjectClass(env, a->clazz), "getWindow", "()Landroid/view/Window;"));
    jobject decor = (*env)->CallObjectMethod(env, win,
        (*env)->GetMethodID(env, (*env)->GetObjectClass(env, win), "getDecorView", "()Landroid/view/View;"));
    // LAYOUT_STABLE | LAYOUT_HIDE_NAVIGATION | LAYOUT_FULLSCREEN | HIDE_NAVIGATION | FULLSCREEN | IMMERSIVE_STICKY
    (*env)->CallVoidMethod(env, decor, (*env)->GetMethodID(env, (*env)->GetObjectClass(env, decor), "setSystemUiVisibility", "(I)V"),
                           0x100 | 0x200 | 0x400 | 0x2 | 0x4 | 0x1000);
    if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionClear(env); ALOG("full screen refused"); }
  }
  glue_on_focus(a, has_focus);
}

void android_main(struct android_app *app) {
  glue_on_focus = app->activity->callbacks->onWindowFocusChanged;
  app->activity->callbacks->onWindowFocusChanged = on_focus_ui;
  app->onAppCmd = on_cmd;
  app->onInputEvent = on_input;
  load(app);
  if (!stream) audio_start();  // once per process, like the game state
  const double frame_s = 1 / 60.0988;
  // Samples to keep queued: enough to cover two of the audio system's pulls, plus a frame.
  const int target = stream ? AAudioStream_getFramesPerBurst(stream) * 2 + APU_HZ / 60 : 0;
  double next = now_s(), stat_t = next;
  long ran = 0, presented = 0;
  int wait_ms = 0;
  static u32 shown[256 * 240];
  for (;;) {
    // Sleep in the event wait until the next frame is due; a button press wakes it at once.
    int events, idle = !(focused && app->window);
    struct android_poll_source *src;
    if (ALooper_pollOnce(idle ? -1 : wait_ms, NULL, &events, (void **)&src) >= 0 && src) src->process(app, src);
    if (app->destroyRequested) { gl_term(); return; }
    if (focused && app->window && !ready) { draw(); wait_ms = 100; continue; }  // red: no ROM
    if (!(focused && app->window)) { next = now_s(); continue; }
    // The wall clock paces frames, one per due time. The audio queue only trims that clock by
    // up to 0.5%, so sound neither runs dry nor piles up, whatever size chunks it is pulled in.
    double t = now_s();
    if (next <= t) {
      pad_state = keys[0] | hat[0] | latch[0]; if (pad_state) npad++;
      pad2_state = keys[1] | hat[1] | latch[1];
      latch[0] = latch[1] = 0;
      frame();
      queue_frame_audio();
      ran++;
      double e = 0;
      if (stream) {
        e = (queued() - target) / (double)(APU_HZ / 60);
        e = e > 1 ? 1 : e < -1 ? -1 : e;
      }
      next += frame_s * (1 + 0.005 * e);
      if (t - next > 0.1) next = t;  // far behind (after a pause): resume, don't race
      if (redraw || memcmp(fb, shown, sizeof shown)) {  // an unchanged frame is not presented
        draw();
        memcpy(shown, fb, sizeof shown);
        redraw = 0;
        presented++;
      }
    }
    if (t - stat_t >= 5) {  // app log lines can get dropped: stats go to a file (run-as cat)
      FILE *f = fopen(stats_path, "w");
      if (f) { fprintf(f, "ran %.2f/s presented %.2f/s queued %d target %d interpreted %ld keys %ld padframes %ld mode %d\n", ran / (t - stat_t),
                       presented / (t - stat_t), queued(), target, interp_rom, nkey, npad, ram[OPER_MODE]); fclose(f); }
      ran = presented = 0; stat_t = t;
    }
    wait_ms = (int)ceil((next - now_s()) * 1000);
    if (wait_ms < 0) wait_ms = 0;
  }
}
