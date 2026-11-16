// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Wii U platform services for the runtime (src/platform/wiiu/wiiu_platform.cpp).
// Only compiled for __WIIU__. See PORTING_NOTES.md (Phase 2, D8, D9, D10).

#include <cstdint>
#include <filesystem>

namespace WiiuPlatform {

// Root of everything the runtime reads or writes on the SD card:
// sd:/WiiCompiled/ (Config.toml, DATA/, NAND/, Logs/).
std::filesystem::path DataRoot();

// Brings the process up, in this order:
//   1. ProcUI (WHBProc) and the on-screen log console (TV + GamePad).
//   2. Pins the calling thread to the codegen core (D9): this thread runs every
//      guest fiber, and code in the codegen area only executes there.
//   3. Creates DataRoot()/Logs and routes stdout/stderr to Logs/console.log
//      (previous run kept as console.prev.log) and to the screen.
//   4. Loads the cold-code image (D8) when the product has one.
// Never returns on failure: it shows the reason on screen and waits for HOME.
void Startup(const char* coldImagePath);

// A guest or host crash caught by the OS exception handler (DSI/ISI/program/
// alignment). Reported from normal thread context, not from the exception.
struct Fault {
    const char* kind;        // "DSI (data access)", "ISI (instruction fetch)", ...
    uint32_t srr0;           // faulting instruction address
    uint32_t dar;            // faulting data address (DSI only)
    uint32_t lr;
    uint32_t core;
    const char* threadName;
};
using FaultReporter = void (*)(const Fault& fault);

// Installs the exception handlers. `reporter` runs on the faulting thread after
// the exception has been left (so it may write files); the process then stops.
void InstallFaultHandler(FaultReporter reporter);

// Shows `message` on screen, keeps the log, and waits for HOME (never returns).
[[noreturn]] void FatalScreen(const char* message);

// Runs ProcUI message processing. Returns false once the user chose to exit.
bool ProcessSystemMessages();

void Shutdown();

} // namespace WiiuPlatform
