# The library must replay the original driver's I2C traffic exactly. The reference is the Gsv2k1 driver in Kramer's
# KDS-EN7 MCU firmware, run under Unicorn by tools/emu.py. The image is not in git; sdk/gsv_sdk.h says where it
# comes from and where to put it. Needs Unicorn in .venv.
CC ?= cc
CFLAGS ?= -std=c11 -O0 -g -Wall -Wextra -Isdk -Iapp -Itests
SDK := $(wildcard sdk/*.c) $(wildcard app/*.c)
IMG := work/kramer/IP5000-A30V0.0.20.bin
EMU := .venv/bin/python tools/emu.py
SCEN := idle plug lock sink hdcp board boardhdcp scale boardpt boardpthdcp boardms boardmshdcp boardfull boardfull22 boardfw boardfw22 boardfwplain

build:
	mkdir -p build
build/replay: tests/replay.c tests/harness.c $(SDK) sdk/*.h app/*.h tests/*.h | build
	$(CC) $(CFLAGS) tests/replay.c tests/harness.c $(SDK) -o $@
build/smoke_board: tests/smoke_board.c $(SDK) sdk/*.h app/*.h | build
	$(CC) $(CFLAGS) tests/smoke_board.c $(SDK) -o $@
build/init.txt: tools/emu.py $(IMG) | build
	$(EMU) init > $@
build/%.txt: tools/emu.py $(IMG) | build
	$(EMU) scenario $* $(TICKS_$*) > $@
TICKS_idle := 6
TICKS_plug := 14
TICKS_lock := 80
TICKS_sink := 120
TICKS_hdcp := 200
TICKS_board := 240
TICKS_boardhdcp := 400
TICKS_scale := 260
ARGS_scale := scaler@120,200
TICKS_boardpt := 300
ARGS_boardpt := scaler@110,-1 passthrough-a
TICKS_boardpthdcp := 420
ARGS_boardpthdcp := scaler@110,-1 passthrough-a
TICKS_boardms := 300
ARGS_boardms := scaler@110,-1 passthrough-a
TICKS_boardmshdcp := 420
TICKS_boardfull := 420
ARGS_boardfull := scaler@110,-1 passthrough-a edid@90
TICKS_boardfull22 := 420
ARGS_boardfull22 := scaler@110,-1 passthrough-a edid@90
TICKS_boardfw := 420
ARGS_boardfw := scaler@110,-1 passthrough-a edid@90 nohdcp-b
TICKS_boardfw22 := 420
ARGS_boardfw22 := scaler@110,-1 passthrough-a edid@90 nohdcp-b
TICKS_boardfwplain := 420
ARGS_boardfwplain := scaler@110,-1 passthrough-a edid@90 nohdcp-b
ARGS_boardmshdcp := scaler@110,-1 passthrough-a
check-init: build/replay build/init.txt
	./build/replay build/init.txt init
check-%: build/replay build/%.txt
	./build/replay build/$*.txt $(TICKS_$*) $(ARGS_$*)
check-board-setup: build/smoke_board
	./build/smoke_board
check: check-init $(addprefix check-,$(SCEN)) check-board-setup
clean:
	rm -rf build
.PHONY: check clean

.SECONDARY:
