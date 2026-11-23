CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -Iinclude
SRCS    := $(wildcard src/*.c)
OBJS    := $(patsubst src/%.c,build/%.o,$(SRCS))

all: build/libhlcore.a

build/%.o: src/%.c | build
	$(CC) $(CFLAGS) -c $< -o $@

build:
	mkdir -p build

build/libhlcore.a: $(OBJS)
	ar rcs $@ $^

build/test_core: tests/test_core.c build/libhlcore.a
	$(CC) $(CFLAGS) $< build/libhlcore.a -lm -o $@

test: build/test_core
	./build/test_core

clean:
	rm -rf build

.PHONY: all test clean sim

# host simulator (tools/host_sim.c + vendored stb_image)
build/host_sim: ../tools/host_sim.c build/libhlcore.a
	$(CC) $(CFLAGS) -Wno-unused-function -I../tools -I../firmware/main $< build/libhlcore.a -lm -o $@

sim: build/host_sim
