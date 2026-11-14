#!/bin/bash
# usage: make test (needs rom/smb.nes). One PASS/FAIL line per check, exits 1 on any FAIL.
# The compiled game must draw exactly the same picture as the interpreter alone (--interp)
# under the same input, and must not have fallen back to the interpreter on the way.
cd "$(dirname "$0")"
B=./build/smb
fail=0
START="200:08,210:00"
# hold Right with B (run), hop with A now and then
RUN="$START,300:82,400:83,420:82,520:83,540:82,640:83,660:82,760:83,780:82,900:00"
for t in "title-demo:2400:" "start:600:$START" "run:1200:$RUN"; do
  IFS=: read -r label frames keys <<< "$t"
  keys=${t#*:*:}
  e=$($B --frames "$frames" ${keys:+--keys "$keys"} --shot build/t-$label.bmp 2>&1)
  $B --frames "$frames" ${keys:+--keys "$keys"} --shot build/t-$label-i.bmp --interp 2>/dev/null
  if cmp -s build/t-$label.bmp build/t-$label-i.bmp; then echo "PASS $label: compiled == interpreted"
  else echo "FAIL $label: compiled and interpreted pictures differ"; fail=1; fi
  case "$e" in *"interpreted: 0 ROM"*) echo "PASS $label: ran fully compiled";;
    *) echo "FAIL $label: $e"; fail=1;; esac
done
# Gameplay reached: $0770 (the game's mode) is 1 after Start
$B --frames 600 --keys "$START" --ramdump build/t.bin 2>/dev/null
m=$(xxd -s 0x770 -l 1 -p build/t.bin)
if [ "$m" = 01 ]; then echo "PASS Start begins the game"; else echo "FAIL Start: mode $m"; fail=1; fi
# Running right moves the level: the picture differs from standing still
if cmp -s build/t-run.bmp build/t-start.bmp; then echo "FAIL running changed nothing"; fail=1; else echo "PASS running scrolls the level"; fi
# Two-player scenarios fly Mario ahead to place things, Luigi alongside him (his controller holds
# the same buttons), so the camera's rule (it waits for the brother behind) holds throughout. The
# few whose subject isn't the camera and that leave Luigi behind set COOPFREECAM=1 (a test aid:
# the camera follows the leader alone, as before that rule).
# Two-player mode (coop.c): player 2 joins with Start at frame 400 and stands still. Luigi must
# show up, and Mario and the camera must move exactly as in the one-player game, frame by
# frame, up to frame 530, where a Goomba kills Luigi (and the level restarts).
K1="200:08,210:00,300:82,475:83,500:82"
RAMLOG=build/t-solo.bin $B --frames 900 --keys "$K1" 2>/dev/null
COOPFREECAM=1 RAMLOG=build/t-coop.bin $B --frames 900 --keys "$K1" --keys2 "400:08,410:00" 2>/dev/null
d=$(python3 -I -c "
import sys; a=open(sys.argv[1],'rb').read(); b=open(sys.argv[2],'rb').read()
print(sum(any(a[f*2048+x]!=b[f*2048+x] for x in (0x6D,0x86,0xB5,0xCE,0x71A,0x71C)) for f in range(530)))" build/t-solo.bin build/t-coop.bin)
if [ "$d" = 0 ]; then echo "PASS coop: Mario and the camera move as in the one-player game"; else echo "FAIL coop: $d frames differ"; fail=1; fi
$B --frames 440 --keys "$K1" --shot build/t-solo440.bmp 2>/dev/null
e=$(COOPFREECAM=1 $B --frames 440 --keys "$K1" --keys2 "400:08,410:00" --shot build/t-coop440.bmp 2>&1)
if cmp -s build/t-solo440.bmp build/t-coop440.bmp; then echo "FAIL coop: Luigi is not on screen"; fail=1; else echo "PASS coop: Luigi is on screen"; fi
case "$e" in *"interpreted: 0 ROM"*) echo "PASS coop: ran fully compiled";; *) echo "FAIL coop: $e"; fail=1;; esac
# Title menu: Select moves to "2 PLAYER GAME", Start: Luigi is there from the start, with no
# input from player 2, and the game runs as one player (no turns: NumberOfPlayers $077A = 0).
$B --frames 440 --keys "100:04,104:00,200:08,210:00" --shot build/t-menu2.bmp --ramdump build/t-menu2.bin 2>/dev/null
$B --frames 440 --keys "200:08,210:00" --shot build/t-menu1.bmp 2>/dev/null
if cmp -s build/t-menu1.bmp build/t-menu2.bmp; then echo "FAIL 2 PLAYER GAME: no Luigi"; fail=1; else echo "PASS 2 PLAYER GAME starts with Luigi"; fi
n=$(xxd -s 0x77A -l 1 -p build/t-menu2.bin)
if [ "$n" = 00 ]; then echo "PASS 2 PLAYER GAME plays both at once (no turns)"; else echo "FAIL players = $n"; fail=1; fi
# With fire power, Luigi (x 24-39) shows none of fiery Mario's red (x 40-55): the game's own
# fiery colours are the same for both.
$B --frames 440 --keys "100:04,104:00,200:08,210:00" --cheats "fire=1" --shot build/t-fire2.bmp 2>/dev/null
n=$(python3 -I -c "
import sys,struct; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
px=lambda x,y: d[o+((239-y)*256+x)*3:o+((239-y)*256+x)*3+3]
red=lambda x0: sum(1 for y in range(176,208) for x in range(x0,x0+16) if px(x,y)==bytes.fromhex('2031b5'))
print(red(24),red(40))" build/t-fire2.bmp)
if [ "${n% *}" = 0 ] && [ "${n#* }" -gt 0 ]; then echo "PASS fiery Luigi is not coloured as fiery Mario"; else echo "FAIL fiery colours: Luigi/Mario red px $n"; fail=1; fi
# --- Two-player scenarios. Mario is moved by test pokes (rt.c --poke: "flying" at a fixed
# height with star power to reach a spot), Luigi placed by COOPSET (coop.c). RAMLOG records RAM
# every frame; ram F ADDR reads one byte of frame F from it.
ram() { python3 -I -c "import sys; d=open(sys.argv[1],'rb').read(); print(d[int(sys.argv[2])*2048+int(sys.argv[3],16)])" build/t-ram.bin "$1" "$2"; }
check() { if [ "$2" = "$3" ]; then echo "PASS $1"; else echo "FAIL $1: got $2, want $3"; fail=1; fi; }
MENU2="100:04,104:00,200:08,210:00"
FLY="00CE:28,380-%d:009F:00,380-%d:00B5:01,380-%d:079F:20"
fly() { printf "380-%d:$FLY" "$1" "$1" "$1" "$1"; }
# Luigi flies along with Mario (he joins at about frame 370), same height, star power
lfly() { echo "375-$1:CE:$2,375-$1:9F:0,375-$1:B5:1,375-$1:79F:20"; }
# The camera never scrolls on past the brother behind: two-player game, Mario runs right, Luigi
# stands at x=24. At frame 500 the screen has stopped with Luigi 8 pixels in from its left edge,
# and Mario waits at its right edge (screen x 239).
e=$(env -u COOPFREECAM COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 501 --keys "100:04,104:00,200:08,210:00,300:82" 2>&1)
cam=$(( $(ram 500 71A) * 256 + $(ram 500 71C) ))
lx=$(echo "$e" | grep '^f500 ' | sed 's/.* x=\([0-9]*\) .*/\1/')
check "the camera waits for the brother behind" "$(( lx - cam )):$(( $(ram 500 6D) * 256 + $(ram 500 86) - cam ))" "8:239"
# Luigi's jump: on the ground under the first ? block, A held for 25 frames. A full jump reaches
# the block (the coin counts) while Mario stands on top of it, and Mario stays standing.
COOPSET="500:6D:1,500:86:0,500:CE:B0,500:9F:0,500:57:0" RAMLOG=build/t-ram.bin $B --frames 600 \
  --keys "$MENU2,370:82,470:00" --keys2 "520:01,545:00" --poke "500-500:006D:01,500-500:0086:00,500-500:00CE:70,500-500:009F:00,500-500:0057:00" 2>/dev/null
check "Luigi's held jump reaches the block (coin)" "$(ram 560 75E)" 1
check "Mario on the bumped block stays standing" "$(ram 545 CE):$(ram 545 1D)" "112:0"
# Star power, each brother's own. Luigi picks up a star (his timer set at frame 380) and the star
# music starts ($F4 is $40 while it plays). It ends with the last star: with Luigi's alone (by
# frame 1499), and not before it when Mario's shorter one has ended (frame 600).
COOPSET="380:79F:23" RAMLOG=build/t-ram.bin $B --frames 1500 --keys "$MENU2" --poke "380-380:00FB:40" 2>/dev/null
check "star music ends when Luigi's star does" "$([ "$(ram 1499 F4)" = 64 ] && echo playing || echo ended)" ended
COOPSET="380:79F:23" RAMLOG=build/t-ram.bin $B --frames 700 --keys "$MENU2" --poke "380-380:00FB:40,380-380:079F:08" 2>/dev/null
check "  and plays on after Mario's has ended" "$(ram 600 F4)" 64
# A brother who joins mid-level has no star or injury timer, though Mario has them.
e=$(COOPLOG=1 $B --frames 450 --keys "200:08,210:00" --keys2 "400:08,410:00" --poke "380-420:079F:80,380-420:079E:80" 2>&1)
check "Luigi joins with no timers of Mario's" "$(echo "$e" | grep '^f440 ' | sed 's/.* hurt=\([0-9]*\) star=\([0-9]*\)/\1:\2/')" "0:0"
# Luigi leaves with a pipe and Mario, the follower, reappears in the coin room with his own star
# (about 24 left), not Luigi's (0x60 at the pipe).
e=$(COOPSET="$(lfly 949 28),950:6D:3,950:86:98,950:CE:70,950:B5:1,950:9F:0,950:57:0,950:1D:0,949:79F:60" COOPLOG=1 $B --frames 1100 \
  --keys "$MENU2,370:82,915:00" --keys2 "370:82,915:00,960:20,1100:00" --poke "$(fly 930)" 2>&1)
st=$(echo "$e" | grep '^f1099 ' | sed 's/.* star=//')
check "Mario keeps his own star into the coin room" "$([ "$st" -gt 0 ] && [ "$st" -le 32 ] && echo own || echo "star $st")" own
# Luigi flashes with his star: his pixels (left of Mario, ground rows) change from one frame to
# the next (the game's cycle for Mario changes every 2 frames), with no star they stand still.
for f in 600 602; do COOPSET="380-700:79F:23" $B --frames $f --keys "$MENU2" --shot build/t-lstar$f.bmp 2>/dev/null; done
for f in 600 602; do $B --frames $f --keys "$MENU2" --shot build/t-lplain$f.bmp 2>/dev/null; done
crop() { python3 -I -c "
import sys,struct,hashlib; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
print(hashlib.md5(b''.join(d[o+((239-y)*256)*3:o+((239-y)*256+100)*3] for y in range(160,216))).hexdigest())" "$1"; }
check "Luigi flashes with his star" "$([ "$(crop build/t-lstar600.bmp)" != "$(crop build/t-lstar602.bmp)" ] && echo flash || echo still)" flash
check "  and stands still without one" "$([ "$(crop build/t-lplain600.bmp)" = "$(crop build/t-lplain602.bmp)" ] && echo still || echo flash)" still
# Flagpole: Mario lands short of the pole, Luigi drifts into it: Luigi takes the pole (slot 0
# sliding, mode 4) and the game moves on to 1-2.
e=$(COOPSET="$(lfly 2439 28),2440:6D:C,2440:86:4E,2440:CE:28,2440:B5:1,2440:9F:0,2440:57:0" COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 3200 \
  --keys "$MENU2,370:82,2420:00" --keys2 "370:82,2420:00,2440:80" --poke "$(fly 2440)" 2>&1)
check "Luigi takes the flagpole: he leads" "$(echo "$e" | grep -m1 -o 'luigi leads')" "luigi leads"
check "  slot 0 slides down the pole" "$(ram 2470 0E)" 4
check "  the game moves on to 1-2" "$(ram 3199 75C)" 1
# ... and Mario, left standing short of the pole, is still shown while Luigi walks to the castle
# (frame 2650: not-sky pixels where he stands, x 40-53, rows 192-207).
COOPSET="$(lfly 2439 28),2440:6D:C,2440:86:4E,2440:CE:28,2440:B5:1,2440:9F:0,2440:57:0" $B --frames 2650 \
  --keys "$MENU2,370:82,2420:00" --keys2 "370:82,2420:00,2440:80" --poke "$(fly 2440)" --shot build/t-flag2650.bmp 2>/dev/null
n=$(python3 -I -c "
import sys,struct; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
px=lambda x,y: d[o+((239-y)*256+x)*3:o+((239-y)*256+x)*3+3]
print(sum(1 for y in range(192,208) for x in range(40,54) if px(x,y)!=px(100,100)))" build/t-flag2650.bmp)
check "  Mario is shown during Luigi's walk to the castle" "$([ "$n" -gt 50 ] && echo shown || echo "$n px")" shown
# Pipe: Luigi stands on 1-1's pipe and presses down: he leads into the coin room (area $A5).
e=$(COOPSET="$(lfly 949 28),950:6D:3,950:86:98,950:CE:70,950:B5:1,950:9F:0,950:57:0,950:1D:0" COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 1100 \
  --keys "$MENU2,370:82,915:00" --keys2 "370:82,915:00,960:20,1100:00" --poke "$(fly 930)" 2>&1)
check "Luigi takes the pipe: he leads" "$(echo "$e" | grep -m1 -o 'luigi leads')" "luigi leads"
check "  into the coin room" "$(ram 1099 750)" 165
# Axe: 1-4 (world/area/level poked on the title), Luigi placed on Bowser's axe: victory mode,
# then World 2-1.
AXE=(--keys "$MENU2,370:82,1225:00" --keys2 "370:82,1225:00" --poke "100-205:075F:00,100-205:0760:04,100-205:075C:03,$(fly 1239 | sed 's/00CE:28/00CE:60/')")
AXESET="$(lfly 1239 60),1240:6D:8,1240:86:C4,1240:CE:60,1240:B5:1,1240:9F:0,1240:57:0"
e=$(WATCH=773 COOPSET="$AXESET" COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 2200 "${AXE[@]}" --shot build/t-axe2200.bmp 2>&1)
check "Luigi takes the axe: he leads" "$(echo "$e" | grep -m1 -o 'luigi leads')" "luigi leads"
check "  victory mode" "$(ram 1241 770)" 2
check "  then World 2-1" "$(ram 2199 75F):$(ram 2199 75C)" "1:0"
# Toad thanks LUIGI: the message the game picks (rt.c WATCH logs the writes to
# VRAM_Buffer_AddrCtrl $0773: $0C "THANK YOU MARIO!", $0D LUIGI).
check "  Toad thanks Luigi" "$(echo "$e" | grep -o 'write \$0773: 00 -> 0[CD]' | sort -u | tr '\n' ' ')" 'write $0773: 00 -> 0D '

# Mario walks to Toad behind Luigi (who stops at x 128): at frame 1800 he stands left of him, lit
# pixels in the black above the floor (rows 176-207, x 0-111).
COOPSET="$AXESET" $B --frames 1800 "${AXE[@]}" --shot build/t-axe1800.bmp 2>/dev/null
n=$(python3 -I -c "
import sys,struct; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
print(sum(1 for y in range(176,208) for x in range(112) if d[o+((239-y)*256+x)*3:o+((239-y)*256+x)*3+3]!=b'\0\0\0'))" build/t-axe1800.bmp)
check "  Mario walks to Toad with him" "$([ "$n" -gt 50 ] && echo shown || echo "$n px")" shown
# The status bar names the leader: in 2-1 Luigi leads, "LUIGI" (top-left, rows 16-23, x 24-63, its
# brightest pixels)
# differs from the "MARIO" of the 2 PLAYER GAME's start.
nm() { python3 -I -c "
import sys,struct; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
p=[d[o+((239-y)*256+x)*3:o+((239-y)*256+x)*3+3] for y in range(16,24) for x in range(24,64)]
print(''.join('#' if c==max(p) else '.' for c in p))" "$1"; }  # the white letters, whatever the sky
check "  the status bar reads LUIGI" "$([ "$(nm build/t-axe2200.bmp)" != "$(nm build/t-menu2.bmp)" ] && echo LUIGI || echo MARIO)" LUIGI
# ...and already on the Toad screen, before any new area loads the status bar again.
COOPSET="$AXESET" $B --frames 1650 "${AXE[@]}" --shot build/t-axe1650.bmp 2>/dev/null
check "  the status bar reads LUIGI above Toad" "$([ "$(nm build/t-axe1650.bmp)" = "$(nm build/t-axe2200.bmp)" ] && echo LUIGI || echo MARIO)" LUIGI
check "  (and MARIO while Mario leads, as for one player)" "$([ "$(nm build/t-menu2.bmp)" = "$(nm build/t-menu1.bmp)" ] && echo MARIO || echo other)" MARIO
# Lives: each brother has his own; any death costs the one who died a life and restarts the
# level, as in the original game.
# Both run right; Mario jumps the first Goomba, Luigi runs into it and is killed: the level restarts with Luigi
# leading on 1 life (the counter shows one less than the lives screen), Mario keeps his 2.
e=$(COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 1000 --keys "$MENU2,300:82,475:83,500:82" --keys2 "300:82" 2>&1)
t=$(ram 800 772); check "Luigi dies: the level restarts (lives screen, not task 3)" "$([ "$t" != 3 ] && echo restart || echo "task $t")" restart
check "  he leads, 1 life; Mario keeps 2" "$(echo "$e" | grep -m1 -o 'luigi leads (lives [0-9]*); follower lives=[0-9]*')" "luigi leads (lives 1); follower lives=2"
# Either brother losing his last life ends the game, as in the original: Mario runs into Goombas
# three times (game over though Luigi has lives left); Luigi on his last life (counter 0, set at
# frame 400) is killed by the first Goomba.
COOPFREECAM=1 RAMLOG=build/t-ram.bin $B --frames 1800 --keys "$MENU2,370:82" 2>/dev/null
check "Mario's last life: game over" "$(ram 1750 770)" 3
COOPFREECAM=1 COOPSET="400:75A:0" RAMLOG=build/t-ram.bin $B --frames 1100 --keys "$MENU2,300:82,475:83,500:82" 2>/dev/null
check "Luigi's last life: game over" "$(ram 1050 770)" 3
# Time running out costs both a life (the timer poked to 000): both counters 2 -> 1; with
# Luigi on his last life, it is game over.
TU="450-450:07F8:00,450-450:07F9:00,450-450:07FA:00"
e=$(COOPLOG=1 $B --frames 1100 --keys "$MENU2" --poke "$TU" 2>&1)
check "time up: both lose a life" "$(echo "$e" | grep '^f1099 ' | grep -o 'lives [0-9]*); follower lives=[0-9]*')" "lives 1); follower lives=1"
COOPSET="400:75A:0" RAMLOG=build/t-ram.bin $B --frames 800 --keys "$MENU2" --poke "$TU" 2>/dev/null
check "time up on Luigi's last life: game over" "$(ram 790 770)" 3
# The lives screen shows both: a second line (icon, "x 3") under Mario's in a two-player game,
# none in a one-player game. Counted: lit pixels in the rows of the second line.
$B --frames 300 --keys "$MENU2" --shot build/t-ls2.bmp 2>/dev/null
$B --frames 300 --keys "200:08,210:00" --shot build/t-ls1.bmp 2>/dev/null
lit() { python3 -I -c "
import sys,struct; d=open(sys.argv[1],'rb').read(); o=struct.unpack('<I',d[10:14])[0]
print(sum(1 for y in range(128,146) for x in range(256) if d[o+((239-y)*256+x)*3:o+((239-y)*256+x)*3+3]!=b'\0\0\0'))" "$1"; }
l2=$(lit build/t-ls2.bmp); l1=$(lit build/t-ls1.bmp)
check "lives screen: Luigi's line in a two-player game" "$([ "$l2" -gt 50 ] && echo shown || echo "$l2 px")" shown
check "lives screen: no second line for one player" "$l1" 0
# Moving platform: 1-3, Mario flies to the sideways platform and stops; Luigi is dropped on it
# at frame 1150 and stands still. It carries him: at frame 1300 (the platform 48 pixels on) he
# stands on it (32 above its top) and 9 pixels in from its left end, where he landed.
e=$(COOPSET="$(lfly 1149 20),1150:6D:5,1150:86:B7,1150:CE:6C,1150:9F:0,1150:57:0,1150:700:0,1150:B5:1,1150:1D:2" COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 1301 \
  --keys "$MENU2,370:82,1120:00" --keys2 "370:82,1120:00" --poke "100-205:075F:00,100-205:0760:03,100-205:075C:02,1120-1300:0057:00,1120-1300:0700:00,$(fly 1300 | sed 's/00CE:28/00CE:20/')" 2>&1)
lx=$(echo "$e" | grep '^f1300 ' | sed 's/.* x=\([0-9]*\) y=\([0-9]*\).*/\1:\2/')
px=$(( $(ram 1300 71) * 256 + $(ram 1300 8A) )); py=$(ram 1300 D2)
check "Luigi rides a moving platform" "$(( ${lx%:*} - px )):$(( py - ${lx#*:} ))" 9:32
# Playground (Up on the title): it stands in for 1-1, with the falling platform at x=816 on
# page 3. Mario flies above it and stops; Luigi alone is dropped on it at frame 950: it falls
# under his weight (it reacts to either brother) and carries him down: at frame 980 he stands
# on it (34 above its top: his turn runs before the platform drops its 2 pixels that frame).
PGM="100:04,104:00,150:10,160:00,200:08,210:00"
e=$(COOPSET="$(lfly 949 30),950:6D:3,950:86:38,950:CE:4C,950:9F:0,950:57:0,950:700:0,950:B5:1,950:1D:2" COOPLOG=1 RAMLOG=build/t-ram.bin $B --frames 981 \
  --keys "$PGM,370:82,900:00" --keys2 "370:82,900:00" --poke "900-980:0057:00,900-980:0700:00,$(fly 980 | sed 's/00CE:28/00CE:30/')" 2>&1)
check "playground: the falling platform is there" "$(ram 950 17):$(ram 950 6F):$(ram 950 88):$(ram 950 D0)" "41:3:48:112"
ly=$(echo "$e" | grep '^f980 ' | sed 's/.* y=\([0-9]*\).*/\1/')
check "  Luigi alone on it: it falls, carrying him" "$([ "$(ram 980 D0)" -gt 144 ] && echo fell):$(( $(ram 980 D0) - ly ))" "fell:34"
# Screen edge and walls (with the camera following Mario alone, COOPFREECAM: the fallback for a
# screen edge that does push him): Luigi stands on the ground left of 1-1's first pipe (x 448-479) with
# star power while Mario flies on. The scrolling screen's left edge pushes Luigi along; once it
# pushes him into the pipe he joins Mario: no frame has his middle inside the pipe (x 440-471
# on the ground). Before, he slid through it.
e=$(COOPFREECAM=1 COOPSET="375-760:79F:20" COOPLOG=1 $B --frames 760 --keys "$MENU2,370:82" --poke "$(fly 759)" 2>&1)
check "the screen edge doesn't push Luigi into a pipe" "$(echo "$e" | grep -cE '^f7[0-5][0-9] .* x=4([4-6][0-9]|7[01]) y=176 ')" 0
check "  he joins Mario instead" "$(echo "$e" | grep '^f750 ' | grep -o 'x=[0-9]* y=[0-9]* yspd=[0-9]* state=[0-9]')" "x=553 y=73 yspd=3 state=2"
# The same with the pipe's lower blocks made bricks ($51): a brick wall stops him too (the
# game's side check: any block but a few, not just the ones that can't be bumped from below).
e=$(COOPFREECAM=1 COOPSET="375-760:79F:20" COOPLOG=1 $B --frames 760 --keys "$MENU2,370:82" \
  --poke "$(fly 759),600-760:066C:51,600-760:066D:51,600-760:067C:51,600-760:067D:51" 2>&1)
check "the screen edge doesn't push Luigi into a brick wall" "$(echo "$e" | grep -cE '^f7[0-5][0-9] .* x=4([4-6][0-9]|7[01]) y=176 ')" 0
# Joining next to a wall: Mario stands just right of that pipe (x=482) in a one-player game and
# player 2 presses Start: Luigi, placed 16 pixels behind him, would be inside the pipe, so he
# appears on Mario instead.
e=$(COOPLOG=1 $B --frames 722 --keys "$START,370:82,690:00" --keys2 "720:08,730:00" \
  --poke "$(fly 699),700-700:006D:01,700-700:0086:E2,700-700:00CE:B0,700-700:009F:00,700-700:0057:00,700-700:0700:00" 2>&1)
check "Luigi joining beside a pipe appears on Mario, not in it" "$(echo "$e" | grep '^f721 ' | grep -o 'x=[0-9]* y=[0-9]*')" "x=482 y=176"
# Fireballs: the brothers share the game's two. Both have fire power (Mario's poked, Luigi's
# set) and press B at frame 450, Luigi facing left, Mario right: the first fireball sets off
# from Luigi (x 24) to the left, the second from Mario (x 40) to the right (each 4 pixels on
# from the player, then a frame's flight).
COOPSET="400:756:2,400:754:0,400:33:2" RAMLOG=build/t-ram.bin $B --frames 460 --keys "$MENU2,450:02,452:00" --keys2 "450:02,452:00" \
  --poke "400-400:0756:02,400-400:0754:00" 2>/dev/null
check "Luigi's fireball: from him, his way" "$(ram 451 24):$(ram 451 8D):$(ram 451 5E)" "1:24:192"
check "  Mario's too: the other one" "$(ram 451 25):$(ram 451 8E):$(ram 451 5F)" "1:48:64"
# Only Luigi has fire power: Mario runs ahead in 1-1 and stops; Luigi throws every 20 frames
# from frame 480. His fireballs fly (though the game moves them only for a fiery leader) and
# shoot the first Goomba (state $22, upside down), 100 points.
K2=$(for f in 480 500 520 540; do printf "%d:02,%d:00," $f $((f+2)); done)
COOPFREECAM=1 COOPSET="400:756:2,400:754:0" RAMLOG=build/t-ram.bin $B --frames 560 --keys "$MENU2,300:82,460:00" --keys2 "${K2%,}" 2>/dev/null
check "  Luigi's fireball shoots a Goomba" "$(ram 559 1E):$(ram 559 7E1)" "34:1"
# Cheats (cheats.c): level select starts the game there (4-2 is area 1, its pipe intro, before the
# underground; 8-4 is area 3), fire power, a stopped clock, and lives that never run out: lives
# and the clock zeroed at frame 400 make the time-up death a game over without the cheat.
cheat() { $B --frames "$1" --keys "$START" --cheats "$2" ${3:+--poke "$3"} --ramdump build/t.bin 2>/dev/null; xxd -p -s "0x$4" -l "$5" build/t.bin; }
check "cheat: level select 4-2" "$(cheat 600 "world=4 level=2" "" 75C 5)" "0101000301"
check "cheat: level select 8-4" "$(cheat 600 "world=8 level=4" "" 75C 5)" "0301000703"
check "cheat: fire power" "$(cheat 600 "fire=1" "" 756 1)" "02"
check "cheat: the clock stops" "$(cheat 700 "time=1" "" 7F8 3)" "$(cheat 1400 "time=1" "" 7F8 3)"
ZERO="400-400:075A:00,400-400:07F8:00,400-400:07F9:00,400-400:07FA:00"
check "cheat: without it, time up at 0 lives ends the game" "$(cheat 1200 "" "$ZERO" 770 1)" "00"
check "cheat: infinite lives, still playing" "$(cheat 1200 "lives=1" "$ZERO" 770 1)" "01"
# The computer's Luigi (--bot): Mario flies across 1-1 and Luigi, on foot, keeps up over its
# pipes, pits and Goombas. Any death of his puts him back by Mario: Mario still leads, and
# nobody lost a life. Controller 2 pressing a button takes Luigi from the computer.
fly_bot() { COOPLOG=1 $B --bot --frames 2350 --keys "$MENU2,370:82,2420:00" --poke "$(fly 2420)" "$@" 2>&1 | grep '^f2349 '; }
l=$(fly_bot)
lx=$(echo "$l" | sed 's/.* x=\([0-9]*\) .*/\1/')
if [ "$lx" -gt 2800 ]; then echo "PASS bot: Luigi keeps up with Mario (x $lx)"; else echo "FAIL bot: Luigi at x $lx"; fail=1; fi
check "bot: Mario leads, no lives lost" "$(echo "$l" | sed 's/^f2349 \(.*\); follower lives=\([0-9]*\) .*/\1 \2/')" "mario leads (lives 2) 2"
lx=$(fly_bot --keys2 "380:01,381:00" | sed 's/.* x=\([0-9]*\) .*/\1/')
if [ "$lx" -lt 100 ]; then echo "PASS bot: controller 2 takes Luigi over"; else echo "FAIL bot: Luigi still driven, x $lx"; fail=1; fi
# Saved progress (save.c). A save of 4-2's underground (area 2) past its checkpoint (page 6):
# Start begins there, at page 6; Left on the title starts from 1-1 instead.
SAVE=build/t-save.txt
printf 'world=3 level=1 area=2 half=6\n' > $SAVE
$B --save $SAVE --frames 600 --keys "$START" --ramdump build/t.bin 2>/dev/null
wla() { echo "$(xxd -p -s 0x75F -l 1 build/t.bin)-$(xxd -p -s 0x75C -l 1 build/t.bin)-$(xxd -p -s 0x760 -l 1 build/t.bin) page $(xxd -p -s 0x71A -l 1 build/t.bin)"; }
check "save: resumes at the level and checkpoint" "$(wla)" "03-01-02 page 06"
$B --save $SAVE --frames 600 --keys "100:40,104:00,$START" --ramdump build/t.bin 2>/dev/null
check "save: Left on the title starts at 1-1" "$(wla)" "00-00-00 page 00"
# Flying through 1-1: past page 5 its checkpoint is saved; over the flagpole, 1-2's pipe intro
# (area 1) from its start, though the screen's page (12) is past 1-2's checkpoint (6) as it loads.
rm -f $SAVE; $B --save $SAVE --frames 3200 --keys "$START,370:82,2420:00" --poke "$(fly 2420)" 2>/dev/null
check "save: 1-1's checkpoint" "$(cat $SAVE)" "world=0 level=0 area=0 half=5"
rm -f $SAVE; $B --save $SAVE --frames 3400 --keys "$START,370:82,2420:00,2440:80" --poke "$(fly 2440)" 2>/dev/null
check "save: the next level" "$(cat $SAVE)" "world=0 level=1 area=1 half=0"
# Recording and replay (replay.c): a two-player run with the computer's Luigi, a save, a cheat
# and controller 2 taking Luigi over, recorded, then replayed with nothing else given: the same
# picture and RAM. A cheat changed mid-run (the clock stopped at frame 500) replays at its frame.
K="$MENU2,300:82,400:83,420:82,520:83,540:82,640:83,660:82,900:00"
printf 'world=0 level=0 area=0 half=5\n' > $SAVE
$B --save $SAVE --bot --cheats "fire=1" --record build/t-rec.txt --frames 1500 --keys "$K" --keys2 "1200:01,1201:00" \
  --shot build/t-rec.bmp --ramdump build/t-rec.bin 2>/dev/null
$B --replay build/t-rec.txt --frames 1500 --shot build/t-rep.bmp --ramdump build/t-rep.bin 2>/dev/null
if cmp -s build/t-rec.bmp build/t-rep.bmp && cmp -s build/t-rec.bin build/t-rep.bin; then echo "PASS replay: the same picture and RAM"
else echo "FAIL replay: differs from the recorded run"; fail=1; fi
# A replay names the build that recorded it: none from the same build, a warning from another.
n=$($B --replay build/t-rec.txt --frames 10 2>&1 | grep -c '^warning: recorded by build')
sed 's/^build .*/build 0000000/' build/t-rec.txt > build/t-rec2.txt
n=$n$($B --replay build/t-rec2.txt --frames 10 2>&1 | grep -c '^warning: recorded by build')
check "replay: warns of a recording from another build" "$n" "01"
printf '0 keys 00 00\n200 keys 08 00\n210 keys 00 00\n500 cheats time=1\n' > build/t-rec.txt
clock() { $B --replay build/t-rec.txt --frames "$1" --ramdump build/t.bin 2>/dev/null; xxd -p -s 0x7F8 -l 3 build/t.bin; }
c450=$(clock 450); c600=$(clock 600)
if [ "$c450" != "$c600" ] && [ "$c600" = "$(clock 900)" ]; then echo "PASS replay: a cheat from its frame on"; else echo "FAIL replay: clock $c450 $c600"; fail=1; fi
# The ROM is data the user supplies; any other file must be refused.
$B --rom testroms/nestest.nes --frames 1 >/dev/null 2>&1
if [ $? = 1 ]; then echo "PASS a different ROM file is refused"; else echo "FAIL a different ROM was accepted"; fail=1; fi
exit $fail
