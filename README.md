# NES static recompiler (Super Mario Bros.) for Android

Translates the 6502 code of an NES ROM into C and builds a native Android app (arm64) from it.
The picture chip, sound chip and controllers are emulated by the runtime (`rt.c`, `apu.c`);
only the game's program is recompiled. The recompiler and runtime come from the Battletoads
recomp (`~/Projects/decomp/battletoads`); the Android frontend from the Tetris DX one.

## What may be shared

This repository holds the tools only: no ROM, no game graphics, no game code. **Do not share
anything under `build/`**: `build/*.gen.c`, `build/*.lst`, the APK and the Mac binary are a
translation of the game's code. The APK contains no ROM data; `make install` copies your own
ROM into the app.

    cp /path/to/your/smb.nes rom/smb.nes
    make test       # nestest CPU check + compiled-vs-interpreted picture comparisons (needs testroms/nestest.*)
    make install    # build/smb.apk -> connected device, ROM copied into the app, launched
    make            # build/smb: Mac build, plays in a window (needs `brew install sdl2-compat`)

Needs the Android SDK at `~/Library/Android/sdk` with the NDK (`RECOMP_SDK` overrides), and a
Java runtime for signing (`brew install openjdk`). No Gradle, no Android Studio.

## Controls (Android)

D-pad moves; A/B is the NES A (jump), X/Y the NES B (run, fire) (Xbox layout: A bottom, X left); Start/R1 is Start, Select/L1
Select. Back pauses during play and leaves the app from the title screen. The game runs full screen (a swipe from
the bottom edge shows the system bar for a moment).

## Two players at once (Luigi)

Choose "2 PLAYER GAME" on the title screen and Luigi plays alongside Mario from the start
(instead of the original game's taking turns); in a one-player game, player 2 can also press
Start during play to join as Luigi (`coop.c`, hooked in by
`mods-smb.txt`). The game itself has one player; Luigi's copy of the player variables is
swapped into its memory, the game's own control, collision and drawing routines run on it, and
it is swapped back out. Luigi is drawn from extra sprite slots in his own colours, so he adds
no flicker. On Android, player 1 is the first controller pressed after launch and any other
controller is player 2; on the Mac, player 2 is W A S D, G (A), F (B), 2 (Start).

Rules:
- Whoever touches a pipe, a vine, the flagpole or Bowser's axe does it: the brothers trade
  places (the "leader" is the one the game itself drives), so the camera and the game's own
  cutscene follow him. The other brother lands and waits, and reappears beside him afterwards.
- The camera follows the leader but never scrolls on past the other brother: a brother who runs
  ahead waits at the right edge of the screen until the other catches up.
  A brother who would join, reappear or be pushed inside a wall or pipe appears on the leader.
- Each brother has his own lives, shown one under the other on the screen before each level.
  Any death goes as in the original game: the one who died loses a life and the level restarts
  from its checkpoint. Time running out costs both a life. Either brother losing his last life ends the game.
- Bumping a block a brother stands on doesn't move him; the block bounces under his feet.
- Moving platforms carry both brothers, and the ones that react to a rider (falling, balance and
  start-when-stepped-on platforms) react to either brother.
- Fire bars hurt either brother, by his own star and injury timers.
- With fire power either brother throws fireballs; they share the game's two (a throw needs the
  next one free).
- The status bar names the leader (MARIO or LUIGI) over the shared score; Toad and the princess
  thank the brother who touched the axe, and the other walks to them with him.

**Computer Luigi**: on Android, "2 PLAYER GAME" with no second controller gives a Luigi the
computer plays (`coop.c`, `bot_pad`; on the Mac, `--bot`): he runs after Mario and jumps walls,
gaps and enemies ahead of him. He costs the game nothing: a death puts him back by Mario
(blinking) with no life lost and no restart, he never takes the lead (pipes, vines, the flagpole
and the axe are Mario's), and left far behind he reappears by Mario. Any button on a second
controller hands Luigi to that player for the rest of the game.

**Playground**: press Up on the title screen (the status bar reads PLAYGROUND; Down undoes it),
then start a game. A test level of our own (`playground.c`) replaces World 1-1: blocks with every
kind of item, a spring, a pipe to the coin room and back, every kind of moving platform, a few
enemies, stairs and the flagpole.

## Saved progress

The game saves the level you reach and whether you passed its checkpoint (the halfway point the
game restarts from after a death) (`save.c`). On the title screen the status bar's WORLD then
shows the saved level, and Start (one or two players) begins there, at the checkpoint if it was
passed, with a new game's lives and score. Left on the title screen starts from 1-1 instead (the
save is then replaced as you play); Right takes the save back. Game over keeps the save. The
playground neither saves nor resumes. Android keeps it in `files/save.txt`; the Mac build saves
only with `--save FILE`.

## Recordings and replays

Every launch of the Android app is recorded from power-on (`replay.c`): the controllers frame by
frame, the save and cheats in force, and whether the computer plays Luigi, a few KB an hour, in
the app's `files/replays/` (one file per launch, named by its date and time; a line `# N game`
marks frame N where a game began). The game is deterministic, so that is all a replay needs.

    make replays                                   # the device's recordings -> build/replays/
    build/smb --replay build/replays/FILE.txt      # play one in the window
    build/smb --replay FILE --start 5000           # skip ahead to frame 5000 (unseen)
    build/smb --replay FILE --frames 5000 --shot f.bmp   # the picture at frame 5000

In the replay window Space pauses and `.` steps one frame. A replay leaves the save alone, and it
needs the build that recorded it: each recording names its commit on its first line (`build
1a2b3c4`), and a replay on another build warns (build that commit to watch it). The Mac build
records with `--record FILE`.

## Cheats (Android)

Settings > Apps > Super Mario Bros. > "Additional settings in the app" (on some phones a gear
icon on the App info page) opens the cheats screen (`android/Cheats.java`, the app's only Java):
infinite lives, the level a new game starts at (any of 1-1 to 8-4), a stopped clock, and fire
power at the start of every life and area. They apply to both brothers and take effect when you
return to the game (level select from the next new game). The game side is `cheats.c`; on the
Mac, `--cheats "lives=1 world=4 level=2 time=1 fire=1"`.

`tools/smbsyms.py` gives the addresses of the names in doppelganger's SMB disassembly
(`ref/smbdis.asm`, which you supply; it is not in this repository) and checks each against the ROM.

## How code gets found

`recomp.py` follows the code from the reset/interrupt vectors and guesses jump-table targets.
Anything it misses runs in the built-in interpreter and is logged to `misses-smb.txt` by the
Mac build; the next build compiles it. `make cover` plays randomly to find more. The app
writes its frame rate and interpreter count to `files/stats.txt`
(`adb shell run-as local.recomp.smb cat files/stats.txt`).

## Limits

- NROM, UxROM, CNROM and AxROM cartridges only (Super Mario Bros. is NROM).
- The picture is drawn a scanline at a time; effects that change mid-line show from the next line.
- Random play only covers early World 1; code first reached later runs in the interpreter
  (correctly, just slower) until a coverage run logs it.

## License

The tools in this repository are under the MIT license (`LICENSE`). They contain no part of the
game: you supply your own ROM. Not affiliated with or endorsed by Nintendo; Super Mario Bros.,
Mario and Luigi are Nintendo's trademarks, named here only to say which game the tools work with.
