# crystal-exec Makefile
#
# Build pipeline:
#   1. Compile crystalexec.dll          (the post-ex DLL, tiny)
#   2. Wrap with Crystal Palace          → crystalexec.pico.bin
#   3. Embed as C array                  → crystalexec_pico.h
#   4. Compile crystal-exec.x64.dll     (has the PICO baked in)
#   5. Copy crystal-exec.x64.dll        → ../  (alongside extension.json)
#
# Requires:
#   CRYSTAL_PALACE_HOME set in environment or ../. crystalenv

CC     := x86_64-w64-mingw32-gcc
CFLAGS := -Wall -Os -DBUILD_DLL
LFLAGS := -shared -Wl,--subsystem,windows

SCRIPT_DIR := $(shell cd .. && pwd)

.PHONY: all clean

all: ../crystal-exec.x64.dll

# ── Step 1: post-ex DLL ────────────────────────────────────────────────────────
crystalexec.dll: crystalexec.c
	$(CC) $(CFLAGS) -o $@ $< $(LFLAGS)

# ── Step 2: Crystal Palace wrap ───────────────────────────────────────────────
crystalexec.pico.bin: crystalexec.dll
	$(SCRIPT_DIR)/generate.sh $< "" $@

# ── Step 3: embed PICO as C array ─────────────────────────────────────────────
crystalexec_pico.h: crystalexec.pico.bin
	xxd -i $< | \
	  sed 's/unsigned char [a-z_A-Z0-9]*/unsigned char crystalexec_pico/; \
	       s/unsigned int [a-z_A-Z0-9]*/unsigned int crystalexec_pico_len/' \
	  > $@

# ── Step 4 + 5: extension DLL with embedded PICO ──────────────────────────────
../crystal-exec.x64.dll: crystal-exec.c crystalexec_pico.h
	$(CC) $(CFLAGS) -o $@ $< $(LFLAGS)

clean:
	rm -f crystalexec.dll crystalexec.pico.bin crystalexec_pico.h \
	      ../crystal-exec.x64.dll
