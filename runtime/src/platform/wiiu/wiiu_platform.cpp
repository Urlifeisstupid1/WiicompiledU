// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wii U platform services: startup, logging to the SD card and the screen, and
// crash reporting through Cafe OS exception callbacks. See wiiu_platform.h.
#include "platform/wiiu_platform.h"

#include "wiicold_loader.h"

#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/mutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

#include <sys/iosupport.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <string>

namespace WiiuPlatform {
namespace {

constexpr const char* kSdRoot = "fs:/vol/external01";
constexpr size_t kScreenColumns = 64;

// ---- log sink: stdout/stderr -> SD file + screen ---------------------------
FILE* g_logFile = nullptr;
OSMutex g_logMutex;
bool g_logReady = false;
std::string g_pendingLine;  // screen output is line-based
OSTime g_lastDraw = 0;

void DrawConsoleThrottled(bool force)
{
    const OSTime now = OSGetSystemTime();
    if (force || OSTicksToMilliseconds(now - g_lastDraw) >= 100) {
        WHBLogConsoleDraw();
        g_lastDraw = now;
    }
}

ssize_t LogWrite(struct _reent* reent, void* fd, const char* data, size_t length)
{
    (void)reent;
    (void)fd;
    if (!g_logReady || length == 0)
        return static_cast<ssize_t>(length);
    OSLockMutex(&g_logMutex);
    if (g_logFile) {
        std::fwrite(data, 1, length, g_logFile);
        std::fflush(g_logFile);
    }
    for (size_t i = 0; i < length; ++i) {
        if (data[i] == '\n') {
            WHBLogPrint(g_pendingLine.c_str());
            g_pendingLine.clear();
        } else if (data[i] != '\r') {
            g_pendingLine.push_back(data[i]);
            // About 66 characters fit across the GamePad screen: wrap long lines
            // instead of cutting them off (continuations are indented).
            if (g_pendingLine.size() >= kScreenColumns) {
                WHBLogPrint(g_pendingLine.c_str());
                g_pendingLine.assign("    ");
            }
        }
    }
    DrawConsoleThrottled(false);
    OSUnlockMutex(&g_logMutex);
    return static_cast<ssize_t>(length);
}

devoptab_t g_logDevoptab{};

void OpenLog()
{
    std::error_code ec;
    const std::filesystem::path logs = DataRoot() / "Logs";
    std::filesystem::create_directories(logs, ec);
    const std::filesystem::path current = logs / "console.log";
    const std::filesystem::path previous = logs / "console.prev.log";
    std::filesystem::remove(previous, ec);
    std::filesystem::rename(current, previous, ec);
    g_logFile = std::fopen(current.c_str(), "w");
    OSInitMutex(&g_logMutex);
    g_logDevoptab.name = "wiicompiled_log";
    g_logDevoptab.write_r = LogWrite;
    devoptab_list[STD_OUT] = &g_logDevoptab;
    devoptab_list[STD_ERR] = &g_logDevoptab;
    setvbuf(stdout, nullptr, _IONBF, 0);
    setvbuf(stderr, nullptr, _IONBF, 0);
    g_logReady = true;
}

// ---- crash reporting ---------------------------------------------------------
FaultReporter g_reporter = nullptr;
Fault g_fault{};
volatile bool g_faultTaken = false;

[[noreturn]] void FaultTrampoline()
{
    // Normal thread context again: safe to write files and draw.
    if (g_reporter)
        g_reporter(g_fault);
    char message[256];
    std::snprintf(message, sizeof(message),
                  "WiiCompiled crashed: %s at 0x%08lX (DAR 0x%08lX, LR 0x%08lX) on core %lu.\n"
                  "Details: sd:/WiiCompiled/Logs/console.log",
                  g_fault.kind, static_cast<unsigned long>(g_fault.srr0), static_cast<unsigned long>(g_fault.dar),
                  static_cast<unsigned long>(g_fault.lr), static_cast<unsigned long>(g_fault.core));
    FatalScreen(message);
}

BOOL TakeFault(OSContext* context, const char* kind)
{
    if (g_faultTaken)
        return FALSE;  // a fault while reporting a fault: let the OS handle it
    g_faultTaken = true;
    OSThread* thread = OSGetCurrentThread();
    g_fault = Fault{kind, context->srr0, context->dar, context->lr, OSGetCoreId(),
                    thread ? OSGetThreadName(thread) : nullptr};
    context->srr0 = reinterpret_cast<uint32_t>(&FaultTrampoline);
    return TRUE;
}

BOOL OnDsi(OSContext* context) { return TakeFault(context, "DSI (data access)"); }
BOOL OnIsi(OSContext* context) { return TakeFault(context, "ISI (instruction fetch)"); }
BOOL OnProgram(OSContext* context) { return TakeFault(context, "program exception (trap / illegal instruction)"); }
BOOL OnAlignment(OSContext* context) { return TakeFault(context, "alignment exception"); }

} // namespace

std::filesystem::path DataRoot()
{
    return std::filesystem::path(kSdRoot) / "WiiCompiled";
}

void Startup(const char* coldImagePath)
{
    WHBProcInit();
    WHBLogConsoleInit();
    WHBLogPrint("WiiCompiled (Wii U) starting");
    WHBLogConsoleDraw();

    // D9: guest code (including the codegen area) only runs on the codegen core.
    const uint32_t codegenCore = OSGetCodegenCore() & 3u;
    OSSetThreadAffinity(OSGetCurrentThread(), static_cast<OSThreadAttributes>(1u << codegenCore));
    OSYieldThread();  // migrate now
    if (OSGetCoreId() != codegenCore) {
        FatalScreen("WiiCompiled: could not move the main thread to the codegen core.");
    }

    OpenLog();
    std::printf("[wiiu] data root %s, main thread on core %lu\n", DataRoot().c_str(),
                static_cast<unsigned long>(OSGetCoreId()));

    std::error_code existsError;
    if (coldImagePath != nullptr && coldImagePath[0] != '\0' &&
        !std::filesystem::exists(coldImagePath, existsError)) {
        std::printf("[wiiu] no cold-code image at %s (expected only for builds without translated "
                    "code in the codegen area)\n", coldImagePath);
    } else if (coldImagePath != nullptr && coldImagePath[0] != '\0') {
        char error[256] = "";
        if (WiiColdLoad(coldImagePath, error, sizeof(error)) != 0) {
            std::string message = std::string("WiiCompiled: could not load the cold-code image ") +
                                  coldImagePath + ":\n" + error;
            FatalScreen(message.c_str());
        }
        std::printf("[wiiu] cold-code image loaded from %s\n", coldImagePath);
    }
}

void InstallFaultHandler(FaultReporter reporter)
{
    g_reporter = reporter;
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, OnDsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, OnIsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, OnProgram);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ALIGNMENT, OnAlignment);
}

void WaitForExit(const char* message)
{
    std::fprintf(stderr, "\n[wiiu] %s\n", message);
    if (g_logFile)
        std::fflush(g_logFile);
    WHBLogPrint("");
    WHBLogPrint("Press HOME to exit.");
    DrawConsoleThrottled(true);
    while (WHBProcIsRunning())
        OSSleepTicks(OSMillisecondsToTicks(100));
}

void FatalScreen(const char* message)
{
    std::string text = std::string("FATAL: ") + message;
    WaitForExit(text.c_str());
    Shutdown();
    std::_Exit(1);
}

bool ProcessSystemMessages()
{
    DrawConsoleThrottled(false);
    return WHBProcIsRunning();
}

void Shutdown()
{
    if (g_logFile) {
        std::fflush(g_logFile);
        std::fclose(g_logFile);
        g_logFile = nullptr;
    }
    g_logReady = false;
    WHBLogConsoleFree();
    WHBProcShutdown();
}

} // namespace WiiuPlatform
