# WiiCompiled → Wii U (Aroma .wuhb) — Porting Notes

Last updated: 2026-11-15 · Upstream base: `patchzyy/wiicompiled` @ `549b14a`, aurora fork `patchzyy/aurora-wiicompiled` (submodule `aurora-main`).

> **Nothing here has run on a Wii U or in Cemu yet.** "Builds" means it compiles and links in this container. Only the user's tests count as "works".

## 1. Current state

| Phase | Status |
|---|---|
| 0 – Toolchain | Done (build only). Bring-up projects are in `wiiu/`, see §2. |
| 1 – Audit | Done. This file. **Waiting on test results (§2) and decisions D1–D3, D5–D6 (§7).** |
| 2 – Headless core | Not started. |
| 3 – GX2 graphics | Not started. |
| 4 – Audio, input, storage | Not started. |

## 2. Phase 0 – toolchain and bring-up builds

**Toolchain:** devkitPPC r51 (GCC 16.2.0, binutils 2.47, newlib 4.6), wut 1.9.2, wut-tools 1.3.5 (elf2rpl, wuhbtool), wiiu-cmake 1.3.2.
pkg.devkitpro.org blocks this cloud container (Cloudflare 403), so `/opt/devkitpro` was copied out of the official `devkitpro/devkitppc:latest` Docker image. On a normal PC use `dkp-pacman -S wiiu-dev wiiu-cmake`.

**Build all bring-up projects:**
```
export DEVKITPRO=/opt/devkitpro DEVKITPPC=$DEVKITPRO/devkitPPC
cmake -B wiiu/build -S wiiu -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
cmake --build wiiu/build
```
The CMake build with the devkitPPC/wut toolchain file works.

| .wuhb | Purpose | Expected result (UNTESTED) |
|---|---|---|
| `hello/wiiu_hello` | Phase 0 hello-world: WHBProc + OSScreen console | Three lines of text, then "tick N" once per second; HOME exits. |
| `tests/stdtest/wiiu_stdtest` | std::thread, mutex/condvar, exceptions, std::filesystem on SD, chrono | `exc ok`, then `tls main=5 a64=5 sd=1 ns=…`; creates `wiicompiled_stdtest.txt` on the SD root. |
| `tests/bigtext/wiiu_bigtext_{64,128,192}mb` | Loader limit for large code (see T4 in §3) | Size line, then `far_fn(4) = 13 (expect 13)`. Report which sizes launch in Cemu and on hardware. |

## 3. Audit findings — blockers found by building

| # | Finding | How it was found | Wii U fix | Size |
|---|---|---|---|---|
| T1 | **`thread_local` cannot be used.** elf2rpl rejects R_PPC_TPREL16_LO/HA (relocs 70/72) under every TLS model. The runtime uses `thread_local` in **27 files**, including hot paths (`g_currentCpuContext`, the FP mirror `g_mkwHostNiActive`). | stdtest link | `MKW_THREAD_LOCAL` macro. Make state touched only by the guest-scheduler thread a plain global (the fiber scheduler is already confined to one host thread). Use `OSGetThreadSpecific`/`OSSetThreadSpecific` for anything that really is per-thread. | M |
| T2 | **No 64-bit atomics.** `__atomic_*_8` is undefined and devkitPPC ships no libatomic. `std::atomic<uint64_t/int64_t/double>` appears in 3 files. | stdtest link | A small libatomic shim using `OSUninterruptibleSpinLock`, or make those counters 32-bit. | S |
| T3 | **The code-size budget is tight** (see §6). | Probe translation | `-Os` for translated shards, less forced inlining, measure in Phase 2. | M |
| T4 | **Huge RPX text.** GNU ld adds long-branch stubs (`lis/addi/mtctr/bctr`) automatically, and elf2rpl accepts 192 MB of text. **It is unknown whether the Cemu or hardware loader accepts it under Aroma.** | bigtext builds | The user tests the bigtext .wuhbs. If they fail: `-Os`, or split into RPLs. | ? |

## 4. Host dependency audit (runtime/ + aurora)

Sizes are relative effort: **S** about a day, **M** a few days, **L** a week or more.

### 4.1 Guest memory model
| Item | Where | Notes | Wii U replacement | Size |
|---|---|---|---|---|
| 4 GiB flat guest reservation at a fixed 64-bit base (`kFixedFlatGuestBase`, `#error` on other arches) | `guest_abi/flat_memory.h`, `guest_flat_memory.cpp` (VirtualAlloc2/MapViewOfFile3, mmap/memfd_create double-mapping, mprotect, fault handlers), `guest_flat_memory_macos.cpp` | Cannot work in a 32-bit address space. Generated code calls only `MemoryInline::Flat*` helpers, so this is replaceable inside the runtime headers without translator changes. | **D1.** Recommendation: route `Flat*` through the existing 1 MiB page-bias tables (`g_fullPageBias` etc. in `memory_access.h`): one table load plus an add per access, with the MMIO write-policy check kept. Guest RAM is plain heap memory (MEM1 24 MB + MEM2, see D6). | M |
| Guest regions | `memory.cpp` `WiiDefaults()` | MEM1, MEM2 (**128 MB**, a dev-kit size; retail Wii is 64 MB), and the uncached/physical aliases point at the same backing memory. The Kamek overlay is 2 MB and the locked cache 16 KB. | Keep the page table. Allocate the backing memory with `MEMAllocFromDefaultHeapEx` (MEM2). | S |
| SEH / `sigaction` + `siglongjmp` crash and fault handling | `main.cpp`, `system_bridge.h` | Used for crash logs and flat-memory faults. | `OSSetExceptionCallbackEx` (DSI/ISI/program) writes a crash log to SD. Fault-based interception is not needed with D1's recommendation. | S |

### 4.2 Endianness (host is now big-endian)
| Item | Where | Wii U fix | Size |
|---|---|---|---|
| Guest load/store byteswap | `memory_access.h` `ByteSwap16/32/64`, `MaybeByteSwap` (19 sites, all centralised) and `memory.cpp` (2) | Make `MaybeByteSwap` the identity when `__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__`. Guest data then loads natively. | S |
| `isa/big_endian.h` (50 uses) | Explicit shift-based codec | Already endian-neutral; no change. | — |
| Paired-single bit swaps | `ppc_isa_quantized.h` (2 `__builtin_bswap64`) | Guard by host byte order. | S |
| AX HLE swaps | `ax_internal.h` (2), `ax_mix.cpp:772` (writes swapped `uint16_t` into guest RAM) | Guard by host byte order. | S |
| `PPC_FPR` union | `cpu_context.h` | Already guarded by `__BYTE_ORDER__`. | — |
| Lane order of packed paired singles | `ppc_isa_float.h` | SSE/NEON code assumes lane 0 is the low 32 bits. The scalar replacement (4.3) must use shifts on the `uint64_t`, which is endian-neutral. | (in 4.3) |
| aurora GX/texture/card byteswaps | `aurora/lib/gx/*`, `gfx/texture_convert.cpp`, `card/*` | aurora's graphics are replaced in Phase 3; nothing to fix there. | — |

### 4.3 x86 / SIMD / host FP
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| Paired-single ops via SSE (`__m128`) / NEON (`float32x2_t`), with `#error` on other arches | `isa/ppc_isa_config.h`, `isa/ppc_isa_float.h` (35 intrinsic sites), `isa/ppc_isa_quantized.h` (49) | **D3.** Add a portable scalar `#else` branch (float ps0/ps1 packed in `uint64_t`). The semantics to keep are NaN pass-through, NI flush, and Gekko rounding. Later option: Espresso's own paired-single instructions through inline asm (Espresso *is* a Broadway derivative). | M |
| FPSCR[NI] mapped to MXCSR FTZ/DAZ or FPCR.FZ | `isa/ppc_isa_fpenv.h`, `cpu_context.h` | Espresso has the **real** FPSCR[NI] bit: `mffs`/`mtfsf`. A closer match than x86. | S |
| `ext_vector_type(2)` (Clang-only) return type `MkwStateFreeResult2` | `ppc_isa_config.h:38`, emitted by `CxxLinearCodeGenerator.StateFreeAbi.cs` | GCC: use a 2×`uint64_t` aggregate with `operator[]` so the emitted `return { a, b };` still compiles. Optionally disable the state-free ABI on PPC32: it was tuned for Win64 register passing. | S |
| `__builtin_rotateleft32` (Clang-only) | `isa/ppc_isa_int.h:14` | Portable rotate (GCC emits `rlwinm`). | S |
| AVX2 AX mix kernels | `hle/audio/ax_mix_kernels.h` | Scalar fallback already exists (`#if __AVX2__`). | — |
| `_mm_pause` spin | `hle/gx/gx_worker.cpp` | `OSYieldThread()`, or nothing. | S |
| `__cpuid` x86-64-v3 baseline check (static constructor) | `host_cpu_baseline.cpp` | Exclude the file. | S |
| `#pragma clang fp contract(off)` | `hle/audio/ax_effects.cpp` | GCC ignores it; compile that file with `-ffp-contract=off` (the translated code already gets `-fno-fast-math -ffp-contract=off`). | S |
| `__regcall`, `__forceinline`, `__declspec` | `ppc_isa_config.h`, `memory_access.h` | Already have GNU-attribute fallbacks for non-Windows. | — |

### 4.4 Fibers / threads / time
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| Guest-thread fibers: Win32 Fibers, libco (Linux/Intel macOS), custom AArch64 asm (Apple) | `host_context.cpp/.h` (a clean 6-function seam), `fiber_manager.cpp` | libco's **`ppc.c` already supports 32-bit big-endian PowerPC SysV** (it saves FPRs; AltiVec code is skipped). Add a `__WIIU__` branch using libco. Needs a check of r2/r13 handling against Cafe's SDA usage. | S–M |
| `std::thread`, mutex, condvar (7 files: GX worker, AX mixer, network deferred, wiimote, music ducking) | various | Links on wut (stdtest). Pin threads to cores via `OSSetThreadAffinity` where it matters (guest on core 1, GX on core 2, audio on core 0). | S |
| `std::chrono` (30 files), timebase emulation | `timebase_contract.h`, `hle/os/os_time.cpp` | Works on newlib/wut. Option: use `OSGetSystemTime` (62.15625 MHz) directly. | S |
| `std::filesystem` (30 files) | NAND emulation, config, DVD root | Links on wut; paths go under `fs:/vol/external01/wiiu/apps/wiicompiled/`. | S |
| Host paths: `GetModuleFileNameW`, `/proc/self/exe`, `$HOME`, `%LOCALAPPDATA%` | `platform/host_platform.cpp`, `runtime_config.h`, `main.cpp` | Fixed SD paths (Config.toml, Logs/, NAND/, DATA/). | S |
| `getenv` (7 files) | various | Returns null on Wii U; check that every caller has a default. | S |

### 4.5 Graphics / windowing (aurora → GX2)
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| aurora (WebGPU via **Dawn**: D3D12/Vulkan/Metal) implements the Dolphin-SDK GX API. The runtime's GX HLE decodes the guest FIFO and calls **135 `GX*` functions** plus about 15 `aurora_*`/`VI*` lifecycle calls. | `hle/gx/*` (24 files), `hle/vi.cpp`, `main.cpp`, `dynamic_aspect.cpp`; aurora `lib/gx`, `lib/gfx`, `lib/dolphin/gx`, pipeline cache (sqlite), `assets/pipeline/initial_pipeline_cache.db` | Phase 3: an **aurora-compatible GX shim on GX2** (same `dolphin/gx/*.h` API), keeping `runtime/src/hle/gx` unchanged. TEV, indirect stages, EFB copies, texture decoding (I4…CMPR into GX2 formats), and TV + DRC scan-out. Shader strategy is **D5**. | **L** |
| Dear ImGui settings bar (F10), controller-mapping wizard, friend-code keyboard | `settings_overlay.cpp` (77 KB), `controller_mapping_wizard.cpp`, `friend_code_keyboard.cpp` | Stub out at first. Later: a minimal GamePad-screen menu, and swkbd for friend codes. | S (stub) |
| Frame interpolation, dynamic aspect, MetalFX, internal-resolution scaling | aurora + `dynamic_aspect.cpp` | Drop. Fixed 854×480 or 1280×720 on TV, with a GamePad mirror. | S |

### 4.6 Audio / input / platform extras (SDL3)
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| SDL3 audio output | `audio_backend.cpp/.h` | wut `sndcore2` (AX): one stereo voice, double-buffered at 48 kHz with resampling from 32 kHz. The AX HLE mixer (`hle/audio/*`) stays. | M |
| SDL3 gamepad / keyboard / mouse / hints / clipboard / joystick | `hle/input/pad.cpp`, `input_bindings.cpp`, `input_expr.cpp`, `motion_trick*.cpp`, `controller_button_names.h`, `steam_input_layout.cpp` | GamePad via `VPADRead`; Pro Controller and Classic Controller via `KPADRead`/`WPAD` mapped to the GameCube pad HLE. Drop Steam Input and the Dolphin-expression UI at first. | M |
| Real Wiimotes: SDL HIDAPI + platform BT (Win32 `bthprops`, Linux BlueZ/DBus via `dlopen`) feeding the emulated IOS Bluetooth stack | `wiimote/*`, `hle/bluetooth/*` | Use `KPAD`/`WPAD` (Wiimote, Nunchuk, Classic, MotionPlus) and feed the existing `hle/bluetooth` emulation (or bypass it at the WPAD level). | M–L |
| GameCube adapter (libusb) | aurora `AuroraLibUSB.cmake` | Drop (optional later through wut USB). | — |
| Discord Rich Presence, music ducking (WinRT SMTC / MPRIS-DBus / CoreAudio) | `discord_presence.cpp`, `music_attenuation.cpp`, `external_audio_macos.cpp` | Exclude. | S |
| `.ico/.icns/.rc` resources | `runtime/resources` | wuhb icon/splash (our own art only). | S |

### 4.7 Storage / network / crypto
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| DVD reads from an **extracted DATA folder** (`ifstream`, FST parse) | `hle/storage/dvd.cpp` | Same code with the root on SD, e.g. `…/wiicompiled/DATA`. The user extracts their own disc; nothing is bundled. Riivolution / Retro Rewind come later. | S |
| NAND/ISFS emulation in host directories | `hle/storage/nand_*.cpp` | Same, under SD. Use FSA for speed later if needed. | S |
| Crypto++ 8.9.0 (vendored, about 150 TUs, `CRYPTOPP_DISABLE_ASM`): sect233r1 ECDSA, SHA-1, `AutoSeededRandomPool` | `include/wii_es_crypto.h`, `third_party/cryptopp` | Keep it. It is portable C++ and supports big-endian PPC. Replace the OS RNG (no `/dev/urandom`) with a seed from `OSGetTime`/`OSGetTick` mixed into `RandomPool`. mbedtls cannot do binary curves, so it is not an alternative. | S |
| mbedtls **3.6.7** (FetchContent) + BSD sockets (`<winsock2>`/`<sys/socket.h>`, `getaddrinfo`, poll) | `hle/net/*` (7 files), `network_poll_contract.h` | wut provides BSD sockets (nsysnet/newlib). Portlibs only has mbedtls **2.28**, so build 3.6.7 for PPC from source. **Online play is deferred until after Phase 4** (offline boot does not need it). | M (deferred) |
| pugixml, toml11 | `third_party/` | Portable; build as-is. | — |

### 4.8 Build system
| Item | Wii U fix | Size |
|---|---|---|
| `runtime/CMakeLists.txt` hard-fails unless the compiler is **64-bit Clang** (`FATAL_ERROR`), and has platform gates for Windows/macOS/Linux only. Aurora is pulled in with Dawn, SDL3, nod, imgui, RmlUi, libusb and sqlite. | Add an `MKW_PLATFORM_WIIU` branch: GCC, 32-bit, no aurora/Dawn/SDL3, link `wut` + `mkw_libco`, and call `wut_create_rpx`/`wut_create_wuhb` in `PublicProducts.cmake`. | M |
| Translation step (.NET 8 translator) | Runs on the PC as today; only the native compile step is cross-compiled. The `.wuhb` contains translated game code: **personal build only, never distributed** (same rule as upstream). | — |

## 5. Does the generated C++ compile with GCC?

**Method:** I built a small homebrew test program with devkitPPC + libogc (int, float, paired-single `psq_l`/`ps_madd`/`ps_sum0`, jump tables, function pointers; **no Nintendo code**). I converted it to a DOL and translated it with the real translator (`translate-recursive` + `generate-data-init`, generic-DOL manifest): **275 functions, 2.5 MB of C++**. The translator itself builds and all 778 tests pass (with .NET SDK 8.0.428; Ubuntu's 8.0.1xx SDK is too old for the Tomlyn source generator).

| Compiler | Result |
|---|---|
| Host Clang (x86-64-v3), the shipped path | Compiles. |
| Host **GCC 13** (x86-64) | 275 errors from **two** Clang-only constructs: `__builtin_rotateleft32` and `ext_vector_type` (`MkwStateFreeResult2`: subscript and brace-return). With both shimmed, **0 errors** across all 275 files plus `data_sections_init.cpp` and `guest_symbol_table.cpp`. |
| **devkitPPC GCC 16.2** | With the two shims applied, the only remaining errors are the three architecture `#error` gates: flat memory base (4.1), SIMD paired-singles (4.3), FP control register (4.3). `data_sections_init_blobs.S` (`.incbin`) assembles with powerpc-eabi-as. |

**Non-portable constructs list:** `ext_vector_type`, `__builtin_rotateleft32`, `#pragma clang fp contract`, x86/NEON intrinsics, `__regcall` (already guarded), `thread_local` (toolchain limitation, T1), 64-bit atomics (T2). **Not used:** `musttail`, `preserve_none`, computed goto, `__int128`, `_BitInt`.

## 6. Size estimate

| Quantity | Value | Basis |
|---|---|---|
| MKWii guest code | **≈ 6.0 MB** PPC (DOL 2.5 MB + StaticR 3.5 MB), 29,792 functions | Gaps between entries in `projects/mkwii/MAP.txt` |
| Native expansion (x86-64, measured on the probe) | `.text` **19.4×** at -O2, **9.5×** at -Os | 81.6 KB of guest code → 1.58 MB (-O2) / 0.77 MB (-Os) |
| Projected x86-64 text for MKWii | ≈ 116 MB (-O2) / ≈ 57 MB (-Os) | ×6.0 MB |
| **Projected Wii U text** | **≈ 150–200 MB (-O2), ≈ 75–100 MB (-Os)** | Assumes PPC32 code is 1.3–1.7× x86-64; MKWii is more float/paired-heavy than the probe. **To be measured in Phase 2.** |
| Embedded guest data (`data_sections_init`) | ≈ 8–9 MB rodata | All DOL+REL sections are embedded |
| Guest RAM | 24 MB MEM1 + 64 MB MEM2 (retail) or 128 MB (current runtime) + 2 MB overlay | `memory.cpp` |
| Runtime + Crypto++ + GX shim | ≈ 5–10 MB code (estimate) | |

**Fit:**
- **Memory:** an Aroma app has several hundred MB of MEM2, so even about 200 MB of text plus about 100 MB of guest RAM plus GPU buffers should fit.
- **Branch range:** fine, because ld adds long-branch stubs automatically (T4).
- **Main risk:** the RPX loader's maximum code size, which is unknown. The bigtext .wuhbs (64/128/192 MB) settle it.
- **Fallback if code is limited to under 100 MB:** `-Os`, lower inlining in the translator's runtime helpers, or splitting translated shards into several RPLs.

## 7. Decisions needed (asking before Phase 2)

| # | Decision | Options | Recommendation |
|---|---|---|---|
| D1 | Guest memory model | A) Page-bias table (exists today, portable, one extra load per access). B) `OSMapMemory` guest window plus a DSI exception handler (fast, complex, Aroma support unknown). | **A** now; revisit B if profiling demands it. |
| D2 | `thread_local` replacement | a) `MKW_THREAD_LOCAL` macro: plain globals for scheduler-thread state, `OSGetThreadSpecific` elsewhere. b) Add TLS relocation support to elf2rpl/wut upstream. | **a**. |
| D3 | Paired-single / FP implementation | a) Portable scalar fallback in the ISA headers (also helps any non-x86/ARM host). b) Espresso inline-asm paired singles. | **a** first, **b** as an optimisation later. |
| D4 | Where the port lives | **Decided:** fork `Urlifeisstupid1/WiicompiledU`, branch `wiiu-port`. Layout: `runtime/src/platform/wiiu/`, `MKW_PLATFORM_WIIU` in CMake, an `aurora`-API GX2 shim library, bring-up projects in `wiiu/`. | — |
| D5 | GX2 shader strategy (Phase 3) | a) Runtime GLSL via CafeGLSL (`glslcompiler.rpl`, which the user installs). b) Precompiled TEV uber-shader (GX2 shader binaries built offline). c) Generated shader variants compiled offline. | Decide at Phase 3; leaning **b** (no extra user files). |
| D6 | Guest MEM2 size | 128 MB (runtime today) vs 64 MB (retail Wii) | 64 MB unless a mod needs more. Check why upstream chose 128. |

## 8. Open problems / risks
1. **RPX loader code-size limit** (T4): unknown until the bigtext tests run.
2. **Performance:** Espresso runs at 1.24 GHz, in-order-ish, no SIMD beyond paired singles. Upstream needs a 3.5 GHz x86 CPU. Translated code that is about 20× expanded may not hold 60 fps. Expect `-O2` on hot shards, D1/D3 optimisations, and possibly 30 fps rendering at first.
3. **GX → GX2** is the largest work item (Phase 3), and aurora's pipeline-cache and EFB-copy paths have no direct equivalent.
4. **Container is temporary:** work is pushed to `Urlifeisstupid1/WiicompiledU` (`wiiu-port`). `/opt/devkitpro` and the .NET 8.0.428 SDK must be reinstalled in each new cloud session (§2).
5. Network (mbedtls 3.x) and Retro Rewind are deferred.
