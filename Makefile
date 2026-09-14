CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra -Wpedantic
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS := $(shell pkg-config --libs raylib)

all: tinyfps

tinyfps: main.c ecs.h tom_thumb_3x5.h
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) -o $@ main.c $(RAYLIB_LIBS) -lm

clean:
	rm -f tinyfps

.PHONY: all clean
