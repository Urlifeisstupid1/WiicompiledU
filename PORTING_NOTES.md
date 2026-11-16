# WiiCompiled → Wii U (Aroma .wuhb) — Porting Notes

Last updated: 2026-11-15 · Repo: `Urlifeisstupid1/WiicompiledU`, branch `wiiu-port` · Upstream base: `patchzyy/wiicompiled` @ `549b14a`, aurora fork `patchzyy/aurora-wiicompiled` (submodule `aurora-main`).

> "Builds" means it compiles and links in this container. Only the user's tests on Cemu or hardware count as "works". Results from the user so far are in §2.

## 1. Current state

| Phase | Status |
|---|---|
| 0 – Toolchain | Done. hello and stdtest **confirmed working on hardware** (user). |
| 1 – Audit | Done. All decisions made (§8). |
| 2 – Headless core | **In progress.** The whole runtime builds and links for the Wii U through CMake (§6.7); a runtime smoke test `.wuhb` is waiting for its hardware run. Remaining: the first boot with your translated game. |
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
| `tests/codegen/wiiu_codegen_probe` round 1 | Is Aroma's 32 MiB codegen (JIT) area usable as overflow space for code? | Area info, sled returns 7, codegen→RPX call returns 141 | **Area at `0x01800000`, 32 MiB, core 1. RPX code loads at `0x0C000000`. `OSCodegenCopy` refused (even 48 bytes).** |
| `tests/codegen/wiiu_codegen_probe` round 2 | Tries `OSCodegenCopy` at other sizes/offsets and the `OSSwitchSecCodeGenMode` RW→memcpy→RX route | `RESULT: codegen area usable…` | **`OSCodegenCopy` refused at every size; `OSSwitchSecCodeGenMode` RW → memcpy → RX works. Codegen→RPX call returned 141; the full 32 MiB nop sled ran (fill 313 ms, run 145 ms).** |
| `tests/overlay/wiiu_overlay_test` round 1 | End-to-end cold-overlay split (D8): RPX↔cold calls, data, small data, bss, rodata, jump table, function-pointer table, libc call, C++ exceptions through cold frames, cold code on all 3 cores | `RESULT: ALL PASSED`; log on SD (`wiicompiled_overlay.txt`) | **Loader refused the image: `bad relocation 24`.** Cause: ld rewrites the cold `.eh_frame` but `--emit-relocs` keeps the pre-rewrite offsets, which pointed past the image. Fixed in the split tool (parses CIEs/FDEs itself; bounds-checks every relocation at build time). |
| `tests/overlay/wiiu_overlay_test` round 2 | Same test with the fixed tool | `RESULT: ALL PASSED` | **All 9 single-core checks pass** (load, both call directions, small data, bss, rodata, jump table, function-pointer table, libc, exceptions both ways through cold frames). **Froze in the all-cores check before logging core 0.** |
| `tests/overlay/wiiu_overlay_test` round 3 | Per core (1, 0, 2): RPX control, then cold code; ISI/DSI handler records the fault; 3 s timeout | Shows whether cold code is executable only on the codegen core (1) | **All single-core checks pass again. RPX code runs on cores 0, 1, 2. Cold code runs on core 1 only; cores 0 and 2 take an ISI at `0x01800064`.** → D9. The test now treats that as the expected result. |
| `wiiu_runtime_smoke` (built by `runtime/` CMake, §6.7) | Upstream's real `RuntimeMain` with no game code: startup on core 1, SD + screen log, Config.toml defaults, guest RAM (MEM1 + 128 MB MEM2 + overlay), system bridge, aurora-wiiu, GX worker thread, then the normal error path | Log reaches `No translated function registered at address 0x...`, then `The runtime stopped with an error`, waits for HOME | **Exactly as expected** (user photo): aurora-wiiu banner, config line, GX worker started, `No translated function registered at address 0x800060a4`, crash artifacts written to `sd:/WiiCompiled/Logs`, clean stop. Long lines were cut off on the GamePad, so lines now wrap at 64 columns. |
| `tests/overlay/wiiu_overlay_test` round 4 | Same checks with cold image v2 (unwind tables in the heap) and the RPX linked at `0x04000000` (§6.9) | `RESULT: ALL PASSED` (cold code on cores 0/2 = expected ISI) | Not tested yet |
| `tests/primitives/wiiu_primitives_test` (new) | `MKW_THREAD_LOCAL` (int, `std::string`, array) on 3 cores at once; 64-bit atomic shim under 3-core contention (`uint64_t` fetch_add touching both halves, `double` CAS loop); runtime `HostContext` fibers on the codegen core (4 contexts x 5000 switches), calling cold-overlay code and keeping FP/integer state across switches | `RESULT: ALL PASSED`; log `wiicompiled_primitives.txt` | **Round 1: thread-locals isolated on all 3 cores; 64-bit atomics exact under contention (80 ms); 20,000 fiber switches on core 1 in 4.9 ms with `IsCurrent` always right; fiber 0's FP/integer state exact. Fibers 1 and 2 reported 0 because of a test bug (a finished fiber returned to the scheduler instead of handing over to the next fiber).** Round 2 (fixed test): **ALL PASSED** (user). |

## 3. Audit findings — blockers found by building

| # | Finding | How it was found | Wii U fix | Size |
|---|---|---|---|---|
| T1 | **`thread_local` cannot be used.** elf2rpl rejects R_PPC_TPREL16_LO/HA (relocs 70/72) under every TLS model. The runtime uses `thread_local` in about **53 declarations** across headers and 18 source files, including hot paths (`g_currentCpuContext`, the FP mirror `g_mkwHostNiActive`, the indirect-dispatch memo). | stdtest link | D2: `MKW_THREAD_LOCAL` with real per-thread storage via `OSGetThreadSpecific` (some state is used from the AX mixer and GX worker threads, so plain globals are not safe everywhere). Hot paths can be optimised afterwards. | M |
| T2 | **No 64-bit atomics.** `__atomic_*_8` is undefined and devkitPPC ships no libatomic. `std::atomic<uint64_t/int64_t/double>` appears in 3 files. | stdtest link | A small libatomic shim using `OSUninterruptibleSpinLock`. | S |
| T3 | **The code-size budget is tight**, but -Os fits (§7). | Probe translation, measured on PPC | Build translated code with `-Os`; spend the leftover budget on `-O2` for hot functions. | M |
| T4 | **RPX code is limited to 64 MiB under Aroma.** Aroma's MochaPayload patches the homebrew title's cos.xml with `max_codesize = 0x04000000` (64 MiB) and `codegen_size = 0x02000000` (a 32 MiB JIT area) (`source/ios_mcp/source/mcp_loadfile.c` in `wiiu-env/MochaPayload`). That matches the 64 MB+ crashes. GNU ld's long-branch stubs work, so branch range is not the problem. | bigtext on hardware + Aroma source | Stay under about 60 MiB of RPX code. The 32 MiB codegen area is a possible overflow for cold code later. **Measured: RPX text up to 28 MB launches; 32 MB hangs (also with incompressible data); 64 MB+ gives 160-0220.** Theory: the 32 MiB codegen area is carved out of the 64 MiB code area, leaving about 32 MB for the RPX including libraries. **Confirmed usable** (`tests/codegen` round 2): the codegen area is at `0x01800000`, 32 MiB, switchable on core 1. RPX code itself loads at `0x0C000000`. Usable executable memory is therefore about **28 MB of RPX + 32 MiB of codegen ≈ 60 MB**, and the split design D8 uses it. | — |

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

### 6.3 Cold overlay (D8) — how it works
- **Build** (`wiiu/cmake/WiiUCold.cmake`): static libraries named `libwiicold_*.a` are cold. A copy of `wut.ld` gets a `coldmem` region at `0x01800000` and the `.wiicold` section (`wiiu/cmake/wiicold.ld`) ahead of `.text`, so it claims the cold libraries' code and their `.eh_frame`. A one-line specs file makes the link use that script.
- **Split** (`wiiu/tools/wiiu_cold_split.py`, Python 3, run after linking and before elf2rpl):
  - Writes `.wiicold` to `cold.bin`, with every relocation from cold code into RPX sections.
  - Drops relocations that point into the cold region, since cold code never moves.
  - The cold `.eh_frame` is parsed directly (CIE personality, FDE pc_begin, LSDA) instead of trusting ld's emitted relocations, whose offsets are stale after ld rewrites `.eh_frame`. Every relocation is bounds-checked against the image at build time.
  - **Fails the build** on any PC-relative reference crossing the two regions. Cold code and its callers are compiled with `-mlongcall`.
  - Removes the section with objcopy.
- **Load** (`wiiu/common/wiicold_loader.c`, `WiiColdLoad`):
  - Rebases the recorded relocations against anchor symbols in `.text/.rodata/.data/.bss`, because the Cafe loader moves the RPX.
  - Switches the area RW → memcpy → RX on the codegen core.
  - Registers the cold `.eh_frame` with `__register_frame_info`, so C++ exceptions can unwind through cold frames.
- **Costs:** `-mlongcall` makes every call 4 instructions instead of 1. Calls between regions are absolute. The PC needs Python 3 for the build (devkitPro's MSYS2 shell: `pacman -S python`).

### 6.4 Runtime primitives (done, confirmed on hardware)
- **T1 `thread_local`:** `include/platform/mkw_thread_local.h` + `src/platform/wiiu/mkw_thread_local.cpp`.
  - `MKW_THREAD_LOCAL(T)` is plain `thread_local T` off the Wii U.
  - On the Wii U it is `MkwThreadLocal<T>`: each OS thread gets a lazily allocated 64 KB arena (OS thread-specific slot 13), each variable owns a slot in it, and each thread's copy is initialised from the prototype on first touch.
  - Operators are forwarded. `MKW_TLS(x)` gives member access (`x` elsewhere, `x.get()` on the Wii U). `MKW_TLS_CONSTINIT` replaces `constinit`. Arrays need a type alias.
  - Applied so far to `host_context.cpp` only. The other ~45 variables are converted while bringing up the runtime build.
- **T2 64-bit atomics:** `src/platform/wiiu/atomic64.c` provides `__atomic_{load,store,exchange,compare_exchange,fetch_*,*_fetch}_8`. It uses 16 address-hashed `OSUninterruptibleSpinLock`s (cross-core safe).
- **HostContext:** the existing libco branch is enabled for `__WIIU__` (libco `ppc.c`: 32-bit SysV, saves r13-r31/f14-f31; the paired-single halves are not saved, which is fine because the runtime never uses paired singles).

### 6.5 Runtime compile sweep (devkitPPC, `-fsyntax-only`, all 100 runtime sources)
| Step | Clean files |
|---|---|
| Upstream code as-is (C++17) | 23 / 100 |
| + `gnu++20` (upstream relies on Clang accepting `constinit`, `std::u8string`, `std::rotr` in C++17 mode), `setjmp` instead of `sigsetjmp` on Wii U | 44 / 100 |
| + all 47 `thread_local`s converted to `MKW_THREAD_LOCAL` (script + 12 use sites with `MKW_TLS`), Wii U graphics-API entry, 32-bit padding in the GX worker command header | 56 / 100 |
| + D10: aurora-wiiu headers, `MKW_HAS_SDL`, Wii U `AudioBackend` (null sink), `WiiuPlatform` + Wii U branches in `main.cpp`, `sd:/WiiCompiled` data root; host-only and network files excluded | **81 / 81 in the Wii U source set** |

Host Clang (x86-64) compiles every runtime file exactly as before these changes. The only failures, `network_ssl.cpp` and `gx_dl.cpp`, are pre-existing: `mbedtls` and `xxhash` are not in this sandbox.

Remaining 44 files, by cause:
- **SDL3 / aurora headers: 32 files.** aurora's own `aurora/event.h` includes SDL3, and the runtime reaches it through `aurora_events.h` from nearly every GX/VI/input file. See D10.
- **Networking: 5 files.** `POLL*` constants from `<poll.h>`. Deferred: stubbed in the first Wii U build.
- **Host-only, excluded from the Wii U build: 7 files.** Flat memory (`guest_flat_memory*.cpp`), Discord, Steam Input layout, the ImGui settings bar and mapping wizard, the friend-code keyboard, and the wiimote host/BT platform files (replaced by KPAD/WPAD in Phase 4).

Other changes made along the way:
- `wii_es_crypto.h`: on Wii U, a per-call Crypto++ `RandomPool` seeded from OS timers replaces `AutoSeededRandomPool` (there is no OS RNG). It is only used to sign local ES identity data.
- `ReadAramByte` now looks up its thread-local window once per call; that's a per-byte TLS lookup on the Wii U, to optimise later.

### 6.6 D10 so far (aurora + SDL3 on the Wii U)
- **`wiiu/aurora-wiiu/include/`** is searched before aurora's include directory.
  - `aurora/event.h`: the same event types without SDL3.
  - `dolphin/gx/GXGeometry.h`: aurora's copy with one change. The C++ `GXSetArray` overload gets `extern "C++"`, because GCC rejects a second C-linkage overload; Clang accepted it. Worth upstreaming to aurora.
- **`include/platform/host_features.h`: `MKW_HAS_SDL`** is 0 on the Wii U and guards the SDL parts of `audio_backend.h` and `pad.cpp`. On the Wii U the PC-only trigger/expression/motion layers are skipped, since aurora-wiiu's `PADRead` reports final statuses.
- **`src/platform/wiiu/audio_backend_wiiu.cpp`:** a null-sink `AudioBackend` (sndcore2 in Phase 4).
- **`include/platform/wiiu_platform.h` + `src/platform/wiiu/wiiu_platform.cpp`:**
  - ProcUI, plus an on-screen log on TV and GamePad.
  - Pins the main (guest) thread to the codegen core (D9).
  - stdout/stderr go to `sd:/WiiCompiled/Logs/console.log` (previous run kept as `console.prev.log`) and to the screen, via a devoptab.
  - Loads the cold image (D8).
  - DSI/ISI/program/alignment faults are caught by OS exception callbacks, the exception is left via an SRR0 trampoline, and the report is written from normal thread context.
- **`main.cpp`:** Wii U branches only; upstream's `RuntimeMain` boot sequence stays the single source of truth.
  - Startup calls `WiiuPlatform::Startup` + the fault handler.
  - No pipe transcript.
  - The fatal popup goes to the log.
  - Graphics-API table: `auto`/`gx2`.
- **Paths:**
  - `RuntimePlatform::ApplicationDataDirectory` = `sd:/WiiCompiled` (Config.toml, DATA/, NAND/, Logs/).
  - `ExecutableDirectory()` = `fs:/vol/content` (the `.wuhb` content folder), so bundled assets (`dsp_coef.bin`, `wii_bootstrap/`) are found "next to the executable" with no other code changes.
- **Excluded from the Wii U build:**
  - `audio_backend.cpp`, `input_bindings.cpp`, `motion_trick.cpp`
  - flat memory, Discord, Steam Input, ImGui overlay/wizard, friend-code keyboard
  - wiimote host/BT platform (KPAD/WPAD in Phase 4)
  - `hle/net/*` (stubbed until online play)

### 6.7 Wii U build (`runtime/cmake/WiiU.cmake`)
`runtime/CMakeLists.txt` switches to it when the devkitPro Wii U toolchain is used, so the PC build paths are untouched.

```
git submodule update --init aurora-main          # only its include/ directory is used
export DEVKITPRO=/opt/devkitpro DEVKITPPC=$DEVKITPRO/devkitPPC
cmake -S runtime -B build-wiiu -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/WiiU.cmake
cmake --build build-wiiu
```

**Libraries:**
- `mkw_wiiu_runtime`: the runtime at `-O2 -ffast-math`. The PPC-semantics files use `-fno-fast-math -ffp-contract=off` as upstream does, and `ax_effects.cpp` gets `-ffp-contract=off` because GCC ignores its Clang pragma.
- `mkw_wiiu_aurora`: aurora-wiiu.
- `mkw_wiiu_cryptopp`: vendored Crypto++ with `NO_OS_DEPENDENCE`; all 150 files build.
- `mkw_wiiu_thirdparty`: pugixml, libco, and xxHash v0.8.4, fetched with the same hash aurora pins.

**Products, `mkw_wiiu_add_product()`:**
- Links the runtime whole-archive (HLE overrides register from static constructors).
- Puts translated `wiicold_*` libraries into the codegen area (D8).
- Strips `.note.GNU-stack` (the translator's blob assembly declares it; elf2rpl rejects it).
- Bundles `dsp_coef.bin` and `wii_bootstrap/` in the `.wuhb` content folder.

**Products built:**
- **`WiiCompiled`:** built when `generated/build_shards/shards.cmake` exists. The shared base shards go to the codegen area; the registration and sensitive shards stay in the RPX. Retro Rewind is not built for the Wii U yet.
- **`wiiu_runtime_smoke`:** always built. It uses `wiiu/tests/runtime-smoke/generated/` (empty data-init and the PAL SDA constants), so it contains no game code.

**aurora-wiiu (Phase 2):**
- `wiiu/tools/gen_aurora_stubs.py` generates the 147 void entry points from aurora's headers (`wiiu/aurora-wiiu/stub_symbols.txt`). Only getters zero their out-parameters; other functions never touch pointer arguments, which may point into guest memory.
- `aurora_wiiu.cpp` holds the value-returning ones:
  - `aurora_initialize` reports success.
  - `aurora_update` returns an empty event list.
  - `GXInit`/`GXGetCPUFifo` return a valid FIFO object.
  - `GXGetTexBufferSize` is ported from aurora (MIT).
  - `PADRead` reports no controllers.

**Other pieces:**
- `src/platform/wiiu/guest_flat_memory_wiiu.cpp`: `GuestFlat` on the Wii U only owns the guest RAM backing. The physical/cached/uncached views of MEM1 and MEM2 alias one heap buffer each, as on PC. There are no protections; everything goes through the checked path (D1).
- `src/platform/wiiu/host_feature_stubs.cpp`: no-op Discord / Steam Input / settings bar / friend-code keyboard. A Wiimote `RemoteHost` with no remotes (Phase 4 implements it on WPAD). The network HLE says "not a network device".
- toml11's parser caches are `static thread_local`. A marked change makes them plain statics on the Wii U only (config is parsed on the main thread).
- `main()` on the Wii U shows the outcome and waits for HOME instead of returning into Aroma.

**Sizes (smoke test, no game code):** RPX text 2.9 MB, data 2.0 MB. That leaves about 25 MB of RPX code space plus the 32 MiB codegen area for translated code.

### 6.8 Building the game: `wiiu/build-game.sh`
Run it on Linux, or in devkitPro's MSYS2 shell on Windows. It:
- checks the tools (devkitPro + `wiiu-cmake`, CMake ≥ 3.25, git, Python 3, .NET 8 SDK);
- checks your `Assets/main.dol` + `Assets/StaticR.rel` with upstream's `Launcher/build-cache.sh` (only clean PAL/NTSC-U/NTSC-J pass);
- builds the translator and runs the same base translation as upstream's `Launcher/local-build.sh` (`translate-recursive`, `emit-base-manifest`, `generate-data-init`, `emit-build-shards`);
- initialises the aurora headers and builds `build-wiiu/WiiCompiled.wuhb`.

`--skip-translation` rebuilds only the `.wuhb`. On the Wii U, `paths.dvd_root` defaults to `sd:/WiiCompiled/DATA`, so no `Config.toml` is needed.

**Tested here with a stand-in for the game:** my homebrew probe's 275 translated functions plus a fake `shards.cmake`, through the real `WiiCompiled` product path. This found and fixed three problems before they could reach a console:
1. The cold-image loader was linked twice.
2. At `-Os`, GCC calls libgcc's out-of-line register save/restore helpers (`_savegpr_*`, `_restgpr_*_x`, `_savefpr_*` …) with plain relative branches that `-mlongcall` does not cover. Fix: `wiiu_cold_library()` gives cold code private renamed copies (`libwiicold_savres.a`, built from libgcc's `crtsav*`/`crtres*` under the GCC Runtime Library Exception).
3. The cold unwind tables contained entries for functions the linker kept in the RPX (inline/template duplicates) and for discarded functions. These are now neutralised (zero range).

### 6.9 Cold image v2 and link layout
- **The unwind tables were larger than the code** (probe: 187 KB vs 147 KB), so they no longer use the 32 MiB executable area:
  - `.wiicold_eh` is linked at `0x80000000`.
  - The loader copies it into heap memory and writes every PC-relative pointer for its real address (relocation type 200).
  - It is then registered with `__register_frame_info`.
- **`cold.bin` v2:** header, anchors, code relocations, unwind-table relocations, code, unwind tables (see `wiiu_cold_split.py`).
- **The RPX's code is now linked at `0x04000000`** instead of wut's `0x02000000`. A game-sized cold section (`0x01800000` + 32 MiB) would otherwise overlap it at link time. The link address is arbitrary because Cafe OS relocates the RPX; the overlay test round 4 confirms the loader accepts it.
- The `coldmem` region is exactly 32 MiB, so a too-large cold section fails at link time with a clear region-overflow error.
- **Verified offline on the probe product:**
  - zero-delta round trip of the code is exact;
  - with the unwind tables placed at an arbitrary heap address, all 276 live FDEs point into cold code;
  - 0 out-of-bounds relocations.

### 6.10 Remaining for Phase 2
1. Hardware run of the overlay test round 4 (new layout).
2. First boot with your game (`wiiu/build-game.sh`).
3. Add the D9 check (translated dispatch asserts it runs on the codegen core in debug builds).
2. `runtime/CMakeLists.txt`: an `MKW_PLATFORM_WIIU` branch (GCC, 32-bit, no aurora/Dawn/SDL3, stubs for GX/VI/audio/input), with `-Os -mlongcall` for translated shards, cold shards as `wiicold_*` libraries via `WiiUCold.cmake` (D8), and `wut_create_rpx`/`wut_create_wuhb`.
3. Platform layer: guest RAM from MEM2, SD paths, crash handler, and a log to the SD card and the screen.
4. Boot test. This needs your own game files. The translator runs on **your PC** against your disc; the .wuhb that comes out contains translated game code, so it is for your own console only and must never be shared.

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
| D6 | Guest MEM2 size | **128 MB, as upstream** (revised). Upstream gives MEM2 the dev-kit size on purpose: Mario Kart Wii detects it and creates its debug expansion heap. The Wii U has the memory, so the port keeps upstream's behaviour. |
| D7 | Code-size strategy | **`-Os` for translated code**, hot functions at `-O2` within the budget. |
| D8 | Using more than ~28 MB of code | **Split build:** the RPX holds the runtime and hot translated code (≤ ~28 MB); the remaining translated code is linked at the codegen address, split out into `cold.bin` in the .wuhb content folder, and copied into the codegen area at startup. See §6.3. |
| D10 | aurora + SDL3 on the Wii U (new, proposed) | **`wiiu/aurora-wiiu/`: a Wii U implementation of the aurora API the runtime uses.** It reuses aurora's Dolphin-SDK headers (`dolphin/gx`, `vi`, `pad`, `mtx`) unchanged and replaces `aurora/event.h`/`aurora.h` with SDL-free versions. Phase 2: no-op GX/VI, null audio, no input, just enough to boot headless. Phase 3 fills the GX side with GX2, Phase 4 adds audio/input. Runtime code that uses SDL directly (SDL gamepads, SDL audio streams, hints, clipboard) is guarded by `MKW_HAS_SDL`, so PC builds are unchanged. Rejected alternative: fake SDL3 headers, which would mean emulating a large API and mislead later work. |
| D9 | Which cores run translated code (forced by hardware) | **Core 1 only** (the codegen core). The guest scheduler thread, which runs every guest fiber, is pinned to core 1. Runtime-only threads (GX worker, AX mixer, I/O) may use cores 0 and 2 but must never call translated code. Upstream already defers guest callbacks (AX resume callbacks, DVD/NAND completions) to the guest thread. Phase 2 adds a debug check on entry to translated dispatch. |

## 9. Open problems / risks
1. **Performance:** Espresso runs at 1.24 GHz with no SIMD beyond paired singles, while upstream asks for a 3.5 GHz x86 CPU. `-Os` code and a checked memory path every access will cost speed. Expect profiling work, selective `-O2`, and possibly native paired-single asm.
2. **Code size:** if the practical limit really is between 16 and 32 MB, the estimated 27–40 MB at `-Os` does not fit in one RPX. Options then: split cold code into RPLs or the 32 MiB codegen area, shrink the translator's inlined helpers, or exclude unused code (e.g. online and Retro Rewind paths).
3. **GX → GX2** is the largest work item (Phase 3).
4. Signalling-NaN corner cases: other float→double widenings (outside the paired-single helpers) keep SNaNs on PowerPC where x86 quiets them. Games essentially never produce SNaNs, so this is only noted.
5. The container is temporary; work is pushed to `wiiu-port`. `/opt/devkitpro` and the .NET SDK must be reinstalled in each new cloud session (§2).
6. Network (mbedtls 3.x) and Retro Rewind are deferred.
