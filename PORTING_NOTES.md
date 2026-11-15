# WiiCompiled → Wii U (Aroma .wuhb) — Porting Notes

Last updated: 2026-11-15 · Repo: `Urlifeisstupid1/WiicompiledU`, branch `wiiu-port` · Upstream base: `patchzyy/wiicompiled` @ `549b14a`, aurora fork `patchzyy/aurora-wiicompiled` (submodule `aurora-main`).

> "Builds" means it compiles and links in this container. Only the user's tests on Cemu or hardware count as "works". Results from the user so far are in §2.

## 1. Current state

| Phase | Status |
|---|---|
| 0 – Toolchain | Done. hello and stdtest **confirmed working on hardware** (user). |
| 1 – Audit | Done. All decisions made (§8). |
| 2 – Headless core | **In progress.** ISA headers, memory model and endianness are done (§6). Remaining: `thread_local`, 64-bit atomics, fibers, CMake platform, SD logging, boot. |
| 3 – GX2 graphics | Not started. |
| 4 – Audio, input, storage | Not started. |

## 2. Phase 0 – toolchain and bring-up builds

**Toolchain:** devkitPPC r51 (GCC 16.2.0, binutils 2.47, newlib 4.6), wut 1.9.2, wut-tools 1.3.5 (elf2rpl, wuhbtool), wiiu-cmake 1.3.2.
pkg.devkitpro.org blocks this cloud container (Cloudflare 403), so `/opt/devkitpro` was copied out of the official `devkitpro/devkitppc:latest` Docker image. On a normal PC use `dkp-pacman -S wiiu-dev wiiu-cmake`. The translator needs .NET SDK **8.0.4xx** (Ubuntu's 8.0.1xx is too old for the Tomlyn source generator); this container used `dotnet-install.sh --channel 8.0` (8.0.428).

**Build all bring-up projects:**
```
export DEVKITPRO=/opt/devkitpro DEVKITPPC=$DEVKITPRO/devkitPPC
cmake -B wiiu/build -S wiiu -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
cmake --build wiiu/build
```

| .wuhb | Purpose | Expected | Result (user, Wii U hardware) |
|---|---|---|---|
| `hello/wiiu_hello` | WHBProc + OSScreen console | Text, "tick N" every second, HOME exits | **OK** |
| `tests/stdtest/wiiu_stdtest` | std::thread, mutex/condvar, exceptions, std::filesystem on SD, chrono | `exc ok`, `tls main=5 a64=5 sd=1 ns=…` | **OK** |
| `tests/bigtext` 64/128/192 MB | Loader code-size limit | `far_fn(4) = 13` | **All three crash, error 160-0220** |
| `tests/bigtext` 16 MB | Loader limit | `far_fn(4) = 13` | **OK** |
| `tests/bigtext` 32 / 56 MB | Loader limit | `far_fn(4) = 13` | **Stuck on the loading screen** |
| `tests/bigtext` 20 / 24 / 28 MB, zero-filled | Find the threshold | `far_fn(4) = 13` | **OK** |
| `tests/bigtext` 32 MB, random-filled | Is the hang caused by the compression ratio? | `far_fn(4) = 13` | **Stuck**: it is the size, not compression |
| `tests/codegen/wiiu_codegen_probe` (new) | Is Aroma's 32 MiB codegen (JIT) area usable as overflow space for code? | Area address/size, full-area sled returns 7, codegen→RPX call returns 141, report on SD (`wiicompiled_codegen.txt`) | Not tested yet |

## 3. Audit findings — blockers found by building

| # | Finding | How it was found | Wii U fix | Size |
|---|---|---|---|---|
| T1 | **`thread_local` cannot be used.** elf2rpl rejects R_PPC_TPREL16_LO/HA (relocs 70/72) under every TLS model. The runtime uses `thread_local` in about **53 declarations** across headers and 18 source files, including hot paths (`g_currentCpuContext`, the FP mirror `g_mkwHostNiActive`, the indirect-dispatch memo). | stdtest link | D2: `MKW_THREAD_LOCAL` with real per-thread storage via `OSGetThreadSpecific` (some state is used from the AX mixer and GX worker threads, so plain globals are not safe everywhere). Hot paths can be optimised afterwards. | M |
| T2 | **No 64-bit atomics.** `__atomic_*_8` is undefined and devkitPPC ships no libatomic. `std::atomic<uint64_t/int64_t/double>` appears in 3 files. | stdtest link | A small libatomic shim using `OSUninterruptibleSpinLock`. | S |
| T3 | **The code-size budget is tight**, but -Os fits (§7). | Probe translation, measured on PPC | Build translated code with `-Os`; spend the leftover budget on `-O2` for hot functions. | M |
| T4 | **RPX code is limited to 64 MiB under Aroma.** Aroma's MochaPayload patches the homebrew title's cos.xml with `max_codesize = 0x04000000` (64 MiB) and `codegen_size = 0x02000000` (a 32 MiB JIT area) (`source/ios_mcp/source/mcp_loadfile.c` in `wiiu-env/MochaPayload`). That matches the 64 MB+ crashes. GNU ld's long-branch stubs work, so branch range is not the problem. | bigtext on hardware + Aroma source | Stay under about 60 MiB of RPX code. The 32 MiB codegen area is a possible overflow for cold code later. **Measured: RPX text up to 28 MB launches; 32 MB hangs (also with incompressible data); 64 MB+ gives 160-0220.** Theory: the 32 MiB codegen area is carved out of the 64 MiB code area, leaving about 32 MB for the RPX including libraries. Being checked with `tests/codegen`. If confirmed, the usable executable memory is about 28 MB of RPX plus 32 MiB of codegen. | — |

## 4. Host dependency audit (runtime/ + aurora)

Items marked **[done]** are implemented on `wiiu-port` (§6).

Sizes are relative effort: **S** about a day, **M** a few days, **L** a week or more.

### 4.1 Guest memory model
| Item | Where | Notes | Wii U replacement | Size |
|---|---|---|---|---|
| 4 GiB flat guest reservation at a fixed 64-bit base (`kFixedFlatGuestBase`, `#error` on other arches) | `guest_abi/flat_memory.h`, `guest_flat_memory.cpp` (VirtualAlloc2/MapViewOfFile3, mmap/memfd_create double-mapping, mprotect, fault handlers), `guest_flat_memory_macos.cpp` | Cannot work in a 32-bit address space. Generated code calls only `MemoryInline::Flat*` helpers, so this is replaceable inside the runtime headers without translator changes. | **[done, headers]** D1: route `Flat*` through the existing 1 MiB page-bias tables (`g_fullPageBias` etc. in `memory_access.h`): one table load plus an add per access, with the MMIO write-policy check kept. Guest RAM is plain heap memory (MEM1 24 MB + MEM2, see D6). | M |
| Guest regions | `memory.cpp` `WiiDefaults()` | MEM1, MEM2 (**128 MB**, a dev-kit size; retail Wii is 64 MB), and the uncached/physical aliases point at the same backing memory. The Kamek overlay is 2 MB and the locked cache 16 KB. | Keep the page table. Allocate the backing memory with `MEMAllocFromDefaultHeapEx` (MEM2). | S |
| SEH / `sigaction` + `siglongjmp` crash and fault handling | `main.cpp`, `system_bridge.h` | Used for crash logs and flat-memory faults. | `OSSetExceptionCallbackEx` (DSI/ISI/program) writes a crash log to SD. Fault-based interception is not needed with D1's recommendation. | S |

### 4.2 Endianness (host is now big-endian)
| Item | Where | Wii U fix | Size |
|---|---|---|---|
| Guest load/store byteswap | `memory_access.h` `ByteSwap16/32/64`, `MaybeByteSwap` (19 sites, all centralised) and `memory.cpp` (2) | **[done]** `MaybeByteSwap` is the identity on a big-endian host. Guest data loads natively. | S |
| `isa/big_endian.h` (50 uses) | Explicit shift-based codec | Already endian-neutral; no change. | — |
| Paired-single bit swaps | `ppc_isa_quantized.h` (2 `__builtin_bswap64`) | **[done]** Guarded by host byte order. | S |
| AX HLE swaps | `ax_internal.h` (2), `ax_mix.cpp:772` (writes swapped `uint16_t` into guest RAM) | Guard by host byte order. | S |
| `PPC_FPR` union | `cpu_context.h` | Already guarded by `__BYTE_ORDER__`. | — |
| Lane order of packed paired singles | `ppc_isa_float.h` | SSE/NEON code assumes lane 0 is the low 32 bits. The scalar replacement (4.3) must use shifts on the `uint64_t`, which is endian-neutral. | (in 4.3) |
| aurora GX/texture/card byteswaps | `aurora/lib/gx/*`, `gfx/texture_convert.cpp`, `card/*` | aurora's graphics are replaced in Phase 3; nothing to fix there. | — |

### 4.3 x86 / SIMD / host FP
| Item | Where | Wii U replacement | Size |
|---|---|---|---|
| Paired-single ops via SSE (`__m128`) / NEON (`float32x2_t`), with `#error` on other arches | `isa/ppc_isa_config.h`, `isa/ppc_isa_float.h` (35 intrinsic sites), `isa/ppc_isa_quantized.h` (49) | **[done]** D3: a portable scalar `#else` branch (lanes kept as raw bits, NaN rules matching x86; verified bit-identical, §6.2). The semantics to keep are NaN pass-through, NI flush, and Gekko rounding. Later option: Espresso's own paired-single instructions through inline asm (Espresso *is* a Broadway derivative). | M |
| FPSCR[NI] mapped to MXCSR FTZ/DAZ or FPCR.FZ | `isa/ppc_isa_fpenv.h`, `cpu_context.h` | **[done]** Espresso has the **real** FPSCR[NI] bit: `mffs`/`mtfsf 0xFF`. A closer match than x86. | S |
| `ext_vector_type(2)` (Clang-only) return type `MkwStateFreeResult2` | `ppc_isa_config.h:38`, emitted by `CxxLinearCodeGenerator.StateFreeAbi.cs` | **[done]** GCC: a 2×`uint64_t` aggregate with `operator[]`, so the emitted `return { a, b };` still compiles. Optionally disable the state-free ABI on PPC32 later: it was tuned for Win64 register passing. | S |
| `__builtin_rotateleft32` (Clang-only) | `isa/ppc_isa_int.h:14` | **[done]** Portable rotate on non-Clang compilers. | S |
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

**Method:** I built a small homebrew test program with devkitPPC + libogc (int, float, paired-single `psq_l`/`ps_madd`/`ps_sum0`, jump tables, function pointers; **no Nintendo code**). I converted it to a DOL and translated it with the real translator (`translate-recursive` + `generate-data-init`, generic-DOL manifest): **275 functions, 2.5 MB of C++**. The translator builds and all 778 of its tests pass.

| Compiler | Upstream headers | `wiiu-port` headers |
|---|---|---|
| Host Clang (x86-64-v3), the shipped path | Compiles | Compiles |
| Host GCC 13 (x86-64) | 275 errors (`__builtin_rotateleft32`, `ext_vector_type`) | **0 errors** |
| devkitPPC GCC 16.2 | Architecture `#error`s (flat memory, SIMD, FP control) | **0 errors** across all 275 functions plus `data_sections_init.cpp` and `guest_symbol_table.cpp` |

**Non-portable constructs:** `ext_vector_type` [fixed], `__builtin_rotateleft32` [fixed], `#pragma clang fp contract` (runtime-only, needs `-ffp-contract=off` on that file), x86/NEON intrinsics [fixed in the ISA headers; `gx_worker.cpp` and `host_cpu_baseline.cpp` remain], `thread_local` (T1), 64-bit atomics (T2). **Not used:** `musttail`, `preserve_none`, computed goto, `__int128`, `_BitInt`.

## 6. Phase 2 progress

### 6.1 Done (header-level; all platforms keep their old code paths)
| Change | Files |
|---|---|
| Portable scalar paired-single backend, used on hosts without SSE/NEON (`MKW_ISA_SCALAR_HOST`). Lanes are raw bit patterns, so lane moves never go through an FP register; NaN results follow the same x86-matching rules as the AArch64 path. Fused ops use the existing exact double-FMA plus tie repair, so they don't depend on Espresso's own `fmadds`. | `isa/ppc_isa_config.h`, `isa/ppc_isa_float.h` |
| psq load/store: the AArch64 scalar bodies became the generic branch; host-order bswaps are guarded by byte order. | `isa/ppc_isa_quantized.h` |
| `PpcWidenPs0Inline`: ps0 widened like x86 `cvtss2sd` (quiets a signalling NaN; PowerPC wouldn't), used by the single-lane psq stores and `PPC_PsToScalar`. | `isa/ppc_isa_float.h`, `isa/ppc_isa_quantized.h` |
| FPSCR[NI] mapped to Espresso's own FPSCR via `mffs`/`mtfsf`. | `isa/ppc_isa_fpenv.h` |
| GCC fallbacks for `__builtin_rotateleft32` and `ext_vector_type`. | `isa/ppc_isa_int.h`, `isa/ppc_isa_config.h` |
| Guest byteswap is the identity on a big-endian host. | `guest_abi/memory_access.h` |
| Memory model D1: on `__WIIU__` there is no flat reservation; every `Flat*` access, including `FlatWriteRam*`, goes through the inline page-bias path (`Memory::Read*/Write*` → `TryReadGuestScalar`). | `guest_abi/flat_memory.h`, `guest_abi/memory_access.h` |

### 6.2 Verification (no Wii U needed)
`runtime/tests/paired_scalar_diff_tests.cpp` (new) runs every ps_* helper over special values (±0, denormals, ±inf, quiet and signalling NaNs with payloads, FLT_MIN/MAX) and random inputs, and prints one hash per operation. It was built for x86-64 (SSE/FMA reference) and for **32-bit big-endian PowerPC Linux run under `qemu-ppc -cpu 750`** (scalar path, BE byte order):
- All **15 operation hashes match** bit-for-bit.
- The repo's `psq_helpers_tests` (5,058,560 checks) passes on both with the **same checksum** (`70b59adfa11ed800`).
- `ppc_pair_fma_tests` passes on both.
- Before `PpcWidenPs0Inline`, 641 single-lane signalling-NaN stores differed; now there are 0.

How to rerun:
```
apt-get install -y g++-powerpc-linux-gnu qemu-user
cd runtime
powerpc-linux-gnu-g++ -std=c++17 -O2 -fno-fast-math -ffp-contract=off -Iinclude -mcpu=750 -static tests/paired_scalar_diff_tests.cpp -o /tmp/pdiff_ppc
qemu-ppc -cpu 750 /tmp/pdiff_ppc   # compare with the same file built by clang++ -march=x86-64-v3
```
Limits: qemu does not model FPSCR[NI] flushing like Espresso, and nothing here has run on Espresso itself.

### 6.3 Remaining for Phase 2
1. T1 `thread_local` → `MKW_THREAD_LOCAL` (OS thread-specific storage).
2. T2 libatomic shim.
3. `HostContext` Wii U backend on libco `ppc.c`.
4. `runtime/CMakeLists.txt`: an `MKW_PLATFORM_WIIU` branch (GCC, 32-bit, no aurora/Dawn/SDL3, stubs for GX/VI/audio/input), with `-Os` for translated shards and `wut_create_rpx`/`wut_create_wuhb`.
5. Platform layer: guest RAM from MEM2, SD paths, crash handler, and a log to the SD card and the screen.
6. Boot test. This needs your own game files. The translator runs on **your PC** against your disc; the .wuhb that comes out contains translated game code, so it is for your own console only and must never be shared.

## 7. Size estimate (now measured on PPC)

The probe's 81,568 bytes of guest code, compiled with devkitPPC GCC 16 for `-mcpu=750`. Template copies (`InvokeDirectCpu<addr>` etc.) are counted once, as the linker keeps one.

| Flags | PPC code | × guest code | Projected for MKWii (≈ 6.0 MB guest code) |
|---|---|---|---|
| `-O2` | 1.40 MB | 17.2× | ≈ 103 MB — does **not** fit 64 MiB |
| `-O1` | 1.22 MB | 14.9× | ≈ 89 MB — does not fit |
| **`-Os`** | **0.37 MB** | **4.5×** | **≈ 27 MB — fits** |

**Plan:** translated shards at `-Os` (estimated 27–40 MB, since MKWii uses more paired-single maths than the probe), plus about 5–10 MB of runtime, leaving roughly 15–25 MB to move hot functions to `-O2` once profiling shows which ones matter. Guest RAM (24 MB MEM1 + 64 MB MEM2 + 2 MB overlay) and the embedded guest data (about 8–9 MB) are data, not code.

## 8. Decisions

The user said "do what you think will work best" (2026-11-15), so the recommendations below are adopted.

| # | Decision | Choice |
|---|---|---|
| D1 | Guest memory model | **Page-bias table** (existing checked path). `OSMapMemory` + DSI handler only if profiling demands it. **[done]** |
| D2 | `thread_local` replacement | **`MKW_THREAD_LOCAL`** with OS thread-specific storage. |
| D3 | Paired-single / FP | **Portable scalar fallback** **[done]**; Espresso inline-asm paired singles as a later optimisation. |
| D4 | Where the port lives | Fork `Urlifeisstupid1/WiicompiledU`, branch `wiiu-port`. |
| D5 | GX2 shader strategy | Decide at Phase 3; leaning toward a precompiled TEV uber-shader. |
| D6 | Guest MEM2 size | **64 MB** (retail), unless a mod turns out to need upstream's 128 MB. |
| D7 | Code-size strategy (new) | **`-Os` for translated code**, hot functions at `-O2` within the 64 MiB budget. |

## 9. Open problems / risks
1. **Performance:** Espresso runs at 1.24 GHz with no SIMD beyond paired singles, while upstream asks for a 3.5 GHz x86 CPU. `-Os` code and a checked memory path every access will cost speed. Expect profiling work, selective `-O2`, and possibly native paired-single asm.
2. **Code size:** if the practical limit really is between 16 and 32 MB, the estimated 27–40 MB at `-Os` does not fit in one RPX. Options then: split cold code into RPLs or the 32 MiB codegen area, shrink the translator's inlined helpers, or exclude unused code (e.g. online and Retro Rewind paths).
3. **GX → GX2** is the largest work item (Phase 3).
4. Signalling-NaN corner cases: other float→double widenings (outside the paired-single helpers) keep SNaNs on PowerPC where x86 quiets them. Games essentially never produce SNaNs, so this is only noted.
5. The container is temporary; work is pushed to `wiiu-port`. `/opt/devkitpro` and the .NET SDK must be reinstalled in each new cloud session (§2).
6. Network (mbedtls 3.x) and Retro Rewind are deferred.
