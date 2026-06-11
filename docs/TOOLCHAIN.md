# Toolchain & Build Pipeline

Build prerequisites, verified versions, and the actual pipeline used to produce PICO blobs and the Sliver Extension DLL.

> Anything marked **verified** was tested locally on macOS Apple Silicon with the listed versions. Linux equivalents (Debian/Ubuntu/Kali) should behave identically.

## 1. Required tools

| Tool | Tested version | Purpose | Install (Debian/Kali) | Install (macOS) |
|---|---|---|---|---|
| `x86_64-w64-mingw32-gcc` | MinGW-w64 GCC 15.2.0 | Cross-compile C sources to Windows x64 objects | `apt install mingw-w64` | `brew install mingw-w64` |
| `nasm` | 3.01 | Assemble `draugr.asm` → `draugr.x64.bin` | `apt install nasm` | `brew install nasm` |
| `java` (JRE) | OpenJDK 17 | Execute `crystalpalace.jar` linker | `apt install default-jdk` | `brew install openjdk@17` |
| `make` | GNU Make ≥ 4 | Build orchestration | preinstalled | preinstalled |
| `xxd` | any | Embed PICO as C byte array (crystal-exec step 3) | `apt install xxd` | preinstalled |
| `zip` | any | Pack operator drop bundle | `apt install zip` | preinstalled |
| `curl` | any | Download Crystal Palace dist | preinstalled | preinstalled |

## 2. External dependencies (not in the repo)

| Artifact | Source | License | How to obtain |
|---|---|---|---|
| `crystalpalace.jar` + `link` wrapper | <https://tradecraftgarden.org/download/cpdist-latest.tgz> | BSD-3-Clause, © 2025 Raphael Mudge / AFF-WG | `curl -fsSL` + `tar -xz` (see §5) |
| `libtcg.x64.zip` | Upstream Crystal-Kit repo | Upstream binary, license unstated (likely derived from QEMU TCG → LGPL/GPL) | `git clone --depth 1 https://github.com/rasta-mouse/Crystal-Kit` then copy |
| Sliver server/client | <https://sliver.sh> | GPLv3 | `curl https://sliver.sh/install \| sudo bash` |

`libtcg.x64.zip` is required at build time because `postex-loader/loader.spec` and `loader/loader.spec` both contain `mergelib "../libtcg.x64.zip"`. The file goes at the same level as `loader/` and `postex-loader/` (i.e. inside `crystal-kit-sliver/`).

## 3. Verified build pipeline

### 3a. Object compilation (per loader)

From `crystal-kit-sliver/loader/Makefile` and `crystal-kit-sliver/postex-loader/Makefile`:

```makefile
CC_64=x86_64-w64-mingw32-gcc
NASM=nasm

$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/loader.c   -o bin/loader.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/services.c -o bin/services.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/pico.c     -o bin/pico.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/hooks.c    -o bin/hooks.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/spoof.c    -o bin/spoof.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/cfg.c      -o bin/cfg.x64.o
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/cleanup.c  -o bin/cleanup.x64.o
# loader/ only:
$(CC_64) -DWIN_X64 -shared -Wall -Wno-pointer-arith -c src/mask.c     -o bin/mask.x64.o
$(NASM) src/draugr.asm -o bin/draugr.x64.bin
```

**Verified outputs:**
- `loader/bin/` → 8 `.o` + 1 `.bin`
- `postex-loader/bin/` → 7 `.o` + 1 `.bin` (no `mask.c`)

### 3b. Crystal Palace link (PICO production)

The upstream Aggressor Script invokes the jar via Java reflection. The bundled `link` wrapper exposes a positional CLI:

```
./link <loader.spec> <file.dll|file.o> <out.bin> [A=hex] [%KEY=value] [@config.spec]
```

Variables relevant for our specs:

| Variable | Type | Used in | Purpose |
|---|---|---|---|
| `%ARGFILE` | string (path) | `postex-loader/loader.spec` | File whose contents become the embedded `dll_args` section read by `DllMain` |

Example invocation:

```bash
./link \
    crystal-kit-sliver/postex-loader/loader.spec \
    /path/to/postex.dll \
    /path/to/out.bin \
    %ARGFILE=/path/to/args.txt
```

**Verified outputs (sizes from local builds):**
- Use case A (`loader/loader.spec` over a Windows DLL) → **117561 bytes** PICO
- Use case B (`postex-loader/loader.spec` over a Windows DLL) → **111741 bytes** PICO

### 3c. Sliver Extension wrapper DLL (`crystal` command)

`crystal-kit-sliver/sliver-glue/wrapper/Makefile`:

```makefile
CC_64 := x86_64-w64-mingw32-gcc
CFLAGS := -Wall -Os -DBUILD_DLL
LDFLAGS := -shared -Wl,--subsystem,windows

# Produces ../crystal-loader.x64.dll
$(CC_64) $(CFLAGS) crystal-loader.c beacon_compatibility.c -o ../crystal-loader.x64.dll $(LDFLAGS)
```

**Verified output:** 114051 bytes, PE32+ x86-64, exports symbol `go`.

A smoke test shellcode (`smoketest.asm`) builds in parallel:

```
nasm -f bin smoketest.asm -o ../build/smoketest.bin
# → 3 bytes: 31 c0 c3  (xor eax,eax; ret)
```

### 3d. crystal-exec — embedded PICO command executor (`crystal-exec` command)

4-step pipeline under `crystal-kit-sliver/sliver-glue/crystal-exec/Makefile`:

```
Step 1: crystalexec.c → crystalexec.dll
  x86_64-w64-mingw32-gcc -Wall -Os -DBUILD_DLL -shared -Wl,--subsystem,windows \
      -o crystalexec.dll crystalexec.c
  NOTE: must be CRT-free (kernel32-only imports). Crystal Palace's manual loader
  resolves imports from the DLL's IAT; msvcrt.dll functions like strtoull/snprintf
  may not resolve correctly in the PICO context and will crash silently.

Step 2: crystalexec.dll → crystalexec.pico.bin  (Crystal Palace wrap)
  ../generate.sh crystalexec.dll "" crystalexec.pico.bin

Step 3: crystalexec.pico.bin → crystalexec_pico.h  (embed as C byte array)
  xxd -i crystalexec.pico.bin | \
    sed 's/unsigned char .../unsigned char crystalexec_pico/; \
         s/unsigned int .../unsigned int crystalexec_pico_len/' > crystalexec_pico.h

Step 4: crystal-exec.c + crystalexec_pico.h → ../crystal-exec.x64.dll
  x86_64-w64-mingw32-gcc -Wall -Os -DBUILD_DLL -shared -Wl,--subsystem,windows \
      -o ../crystal-exec.x64.dll crystal-exec.c
```

Key design constraints:
- `crystalexec.c` must import **only from kernel32.dll** — no msvcrt.dll / CRT functions
- Use custom string helpers (`hex_parse`, `str_append`) instead of `strtoull`, `strtol`, `snprintf`
- `crystal-exec.c` (the Sliver extension itself) may use CRT normally — it runs in the normal process context, not inside the PICO loader
- Single callback model: all output accumulates into a heap buffer; exactly ONE `callback(buf, len)` call at the very end. Sliver extension loaders only display the first callback invocation — any subsequent calls are silently dropped.

**Verified output:** `crystal-exec.x64.dll` — PE32+ x86-64, exports symbol `go`.

## 4. End-to-end timing on macOS Apple Silicon (reference)

| Step | Elapsed |
|---|---|
| `apt`/`brew install mingw-w64 nasm openjdk@17` | ~3 min |
| Download `cpdist-latest.tgz` (1.8 MB) | ~5 s |
| `make -C loader all` | ~2 s |
| `make -C postex-loader all` | ~2 s |
| `make -C sliver-glue/wrapper all` | ~3 s |
| `make -C sliver-glue/crystal-exec all` (4-step, includes Crystal Palace wrap) | ~4 s |
| `./link` Crystal Palace run | ~1 s |
| `./bundle-implant.sh` (zip) | <1 s |

## 5. One-time setup script (Kali / Debian / Ubuntu)

```bash
#!/usr/bin/env bash
set -e

# 1. Toolchain
sudo apt update
sudo apt install -y mingw-w64 nasm default-jdk make zip git curl

# 2. Crystal Palace
mkdir -p external/crystalpalace
curl -fsSL https://tradecraftgarden.org/download/cpdist-latest.tgz \
   | tar -xz -C external/crystalpalace/
chmod +x external/crystalpalace/dist/{link,piclink,coffparse,linkserve}
export CRYSTAL_PALACE_HOME=$(pwd)/external/crystalpalace/dist

# 3. libtcg from upstream
git clone --depth 1 https://github.com/rasta-mouse/Crystal-Kit /tmp/ck
cp /tmp/ck/libtcg.x64.zip crystal-kit-sliver/
rm -rf /tmp/ck

# 4. Verify
x86_64-w64-mingw32-gcc --version | head -1
nasm --version
java -version
ls -la "$CRYSTAL_PALACE_HOME/crystalpalace.jar" crystal-kit-sliver/libtcg.x64.zip

echo "Setup OK. Now: cd crystal-kit-sliver && make -C loader all && make -C postex-loader all"
```

## 6. Required environment variables

| Variable | Required for | Default behaviour |
|---|---|---|
| `CRYSTAL_PALACE_HOME` | every script under `sliver-glue/` that calls `./link` | scripts exit with error if unset |
| `SLIVER_SERVER` | `generate-implant.sh --profile` mode only | defaults to `sliver-server` in `$PATH` |

## 7. Known toolchain gotchas

- macOS rosetta vs ARM64: brew installs native ARM binaries; the cross-compiled output is `x86-64` Windows PE so this works regardless.
- MinGW-w64 ≥ 13 introduces `-fcf-protection` defaults that can break PIC. The Makefiles pass `-shared -Wno-pointer-arith`, no extra hardening flags. If your distro pins a different MinGW build, verify with `make` first.
- `./link` and friends inside the Crystal Palace dist must be executable. The tarball usually preserves the bit, but after `tar -xz` some filesystems strip exec; fix with `chmod +x external/crystalpalace/dist/{link,piclink,coffparse,linkserve}`.
- `libtcg.x64.zip` must be located at `crystal-kit-sliver/libtcg.x64.zip` (sibling of `loader/`). The spec files reference it as `../libtcg.x64.zip`.

## 8. Compiler flags rationale

The `-Wno-pointer-arith` flag is required because Crystal-Kit's loader code performs pointer arithmetic on `void*` (a GNU extension that GCC warns about by default but does not break).

The `-shared` flag instructs the compiler to emit relocatable code suitable for being merged by Crystal Palace into a single position-independent blob. This does NOT produce a Windows DLL on its own — the actual `.o` outputs are intermediate.

`-DWIN_X64` is consumed by `loader.h` to conditionally select 64-bit-specific struct layouts and macros.
