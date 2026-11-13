# make                  -> build/smb from rom/smb.nes (Mac; plays in a window, or headless with --frames)
# make check            -> CPU check against the nestest reference log
# make test             -> check + compiled-vs-interpreted picture comparisons of the game
# make cover            -> random-play coverage; then `make` compiles whatever it found
# make apk              -> build/smb.apk (Android, arm64)
# make install          -> install the APK, copy the ROM into the app, launch it
ROM ?= rom/smb.nes
NAME ?= smb
MISSES = misses-$(NAME).txt
MODS = $(if $(filter smb,$(NAME)),mods-smb.txt)

build/$(NAME): rt.c rt.h apu.c coop.c playground.c cheats.c save.c replay.c recomp.py $(ROM) $(wildcard $(MISSES)) $(MODS)
	mkdir -p build
	python3 -I recomp.py $(ROM) build/$(NAME).gen.c $(MISSES) $(RECOMP_ARGS) $(if $(MODS),--mods $(MODS)) --lst build/$(NAME).lst
	clang -O0 -I. -c build/$(NAME).gen.c -o build/$(NAME).gen.o
	clang -O2 $$(sdl2-config --cflags) -DMISSES='"$(MISSES)"' -DNAME='"$(NAME)"' -DROM_DEFAULT='"$(ROM)"' -DBUILD='"$(shell git describe --always --dirty)"' rt.c build/$(NAME).gen.o -o $@ $$(sdl2-config --libs)

check:
	$(MAKE) ROM=testroms/nestest.nes NAME=nestest RECOMP_ARGS="--entry C000"
	python3 -I check_nestest.py

test: check build/smb
	bash test.sh

cover: build/smb
	python3 -I tools/cover.py

apk:
	bash android/build.sh $(ROM) $(NAME)

install: apk
	bash android/install.sh $(ROM) $(NAME)

# The device's recordings (replay.c) -> build/replays/; play one with build/smb --replay FILE
replays:
	rm -rf build/files && mkdir -p build/replays
	adb exec-out run-as local.recomp.$(NAME) tar c files/replays | tar x -C build
	mv build/files/replays/* build/replays/ && rm -rf build/files && ls build/replays

.PHONY: check test cover apk install replays
