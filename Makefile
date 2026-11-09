CC ?= cc
CFLAGS ?= -std=c11 -O2 -Wall -Wextra
RAYLIB_CFLAGS := $(shell pkg-config --cflags raylib)
RAYLIB_LIBS := $(shell pkg-config --libs raylib)

all: voidryl

voidryl: main.c client.c server.c shared.c networking.c models.c
	$(CC) $(CFLAGS) $(RAYLIB_CFLAGS) main.c -o $@ $(RAYLIB_LIBS) -lm

.PHONY: all
