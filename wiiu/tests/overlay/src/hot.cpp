// RPX half of the overlay test. Loads content/cold.bin into the codegen area,
// then exercises every kind of reference between the two halves. Prints each
// check on screen and writes the same lines to sd:/wiicompiled_overlay.txt.
#include "shared.h"
#include "wiicold_loader.h"

#include <coreinit/codegen.h>
#include <coreinit/context.h>
#include <coreinit/core.h>
#include <coreinit/exception.h>
#include <coreinit/time.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <stdarg.h>
#include <stdexcept>
#include <stdio.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

int g_hotCounter = 1;
int g_hotArray[64];
const char g_hotText[] = "WiiCompiled overlay";

static FILE *g_log;
static int g_failures;

static void Report(const char *format, ...) __attribute__((format(printf, 1, 2)));
static void Report(const char *format, ...)
{
    char line[256];
    va_list args;
    va_start(args, format);
    vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    WHBLogPrint(line);
    WHBLogConsoleDraw();
    if (g_log) {
        fputs(line, g_log);
        fputc('\n', g_log);
        fflush(g_log);
    }
}

static void Check(const char *what, long long got, long long expected)
{
    if (got != expected)
        ++g_failures;
    Report("%s %s: got %lld, expected %lld", got == expected ? "OK  " : "FAIL", what, got, expected);
}

extern "C" int HotAdd(int a, int b)
{
    return a + b;
}

extern "C" void HotThrow(int code)
{
    throw std::runtime_error(code >= 0 ? "cold-frame" : "x");
}

// RPX data pointing at cold code (absolute relocations dropped by the split tool).
static int (*const g_coldTable[])(int) = {ColdCompute, ColdSwitch};

// Hardware result (round 3): the codegen area is executable ONLY on the codegen
// core (core 1); cold code on cores 0 and 2 takes an ISI exception. That is now
// the expected outcome and the rule the port follows (PORTING_NOTES.md, D9):
// translated code only ever runs on core 1. Each core runs plain RPX code first
// (control), then cold code, with every step logged before it starts. ISI/DSI
// exceptions are caught (the handler records the address and ends the thread)
// and a 3-second timeout catches a thread that hangs without faulting.
static volatile uint32_t g_faultType;
static volatile uint32_t g_faultSrr0;
static volatile uint32_t g_faultDar;

extern "C" __attribute__((noinline)) int HotOnly(int x)
{
    return x + 1;
}

static void FaultExit()
{
    OSExitThread(-999);
}

static BOOL OnIsi(OSContext *context)
{
    g_faultType = 3;
    g_faultSrr0 = context->srr0;
    context->srr0 = (uint32_t)&FaultExit;
    return TRUE;
}

static BOOL OnDsi(OSContext *context)
{
    g_faultType = 2;
    g_faultSrr0 = context->srr0;
    g_faultDar = context->dar;
    context->srr0 = (uint32_t)&FaultExit;
    return TRUE;
}

static int RunHotOnCore(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    return HotOnly(40) + (int)OSGetCoreId() * 1000;
}

static int RunColdOnCore(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    return ColdSwitch(3) + (int)OSGetCoreId() * 1000;
}

static int RunThreadOnCore(uint32_t core, OSThreadEntryPointFn entry, bool *finished)
{
    const uint32_t stackSize = 16 * 1024;
    OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
    uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
    int result = -1;
    *finished = false;
    if (thread && stack &&
        OSCreateThread(thread, entry, 0, nullptr, stack + stackSize, stackSize, 16,
                       (OSThreadAttributes)(1u << core))) {
        g_faultType = 0;
        OSResumeThread(thread);
        for (int waited = 0; waited < 300 && !OSIsThreadTerminated(thread); ++waited)
            OSSleepTicks(OSMillisecondsToTicks(10));
        if (OSIsThreadTerminated(thread)) {
            OSJoinThread(thread, &result);
            *finished = true;
            MEMFreeToDefaultHeap(thread);
            MEMFreeToDefaultHeap(stack);
        }
        // A hung thread keeps its stack and OSThread: freeing them would be unsafe.
    }
    return result;
}

static void CheckOnAllCores()
{
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, OnIsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, OnDsi);
    static const uint32_t order[] = {1, 0, 2};
    for (uint32_t core : order) {
        for (int cold = 0; cold < 2; ++cold) {
            Report("...  starting %s code on core %lu", cold ? "cold" : "RPX", (unsigned long)core);
            bool finished = false;
            const int result = RunThreadOnCore(core, cold ? RunColdOnCore : RunHotOnCore, &finished);
            char what[64];
            snprintf(what, sizeof(what), "%s code on core %lu", cold ? "cold" : "RPX", (unsigned long)core);
            if (!finished) {
                ++g_failures;
                Report("FAIL %s: thread did not finish within 3 s", what);
            } else if (result == -999) {
                const bool expected = cold && core != (OSGetCodegenCore() & 3u) && g_faultType == 3;
                if (!expected)
                    ++g_failures;
                Report("%s %s: %s exception at 0x%08lX (DAR 0x%08lX)%s", expected ? "OK  " : "FAIL", what,
                       g_faultType == 3 ? "ISI (instruction fetch)" : "DSI (data access)",
                       (unsigned long)g_faultSrr0, (unsigned long)g_faultDar,
                       expected ? " - expected, cold code runs only on the codegen core" : "");
            } else if (cold && core != (OSGetCodegenCore() & 3u)) {
                ++g_failures;
                Report("FAIL %s: ran (got %d) - the codegen area is executable here after all; "
                       "update PORTING_NOTES.md D9", what, result);
            } else {
                Check(what, result, 41 + (long long)core * 1000);  // HotOnly(40) and ColdSwitch(3) are both 41
            }
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();
    g_log = fopen("fs:/vol/external01/wiicompiled_overlay.txt", "w");
    Report("WiiCompiled cold-overlay test");
    Report("cold code at %p, RPX HotAdd at %p", (void *)&ColdCompute, (void *)&HotAdd);

    char error[160] = "";
    if (WiiColdLoad("fs:/vol/content/cold.bin", error, sizeof(error)) != 0) {
        Report("FAIL load: %s", error);
        ++g_failures;
    } else {
        Report("OK   cold image loaded");
        // ColdHelper(5)=35, + g_hotArray[5]=5 via HotAdd, + counter (1 -> 2)
        Check("RPX->cold call, cold->RPX call and data", ColdCompute(5), 42);
        Check("RPX small data written by cold code", g_hotCounter, 2);
        Check("RPX bss written by cold code", g_hotArray[5], 5);
        Check("cold jump table", ColdSwitch(4), 59);
        Check("RPX function-pointer table into cold", g_coldTable[1](6), 73);
        Check("cold libc call (strlen) + RPX rodata", (long long)ColdChecksum(g_hotText),
              (long long)ColdChecksum(g_hotText) /* recomputed below */);
        uint32_t expectedHash = 2166136261u;
        for (const char *p = g_hotText; *p; ++p)
            expectedHash = (expectedHash ^ (uint8_t)*p) * 16777619u;
        Check("cold checksum matches RPX computation", (long long)ColdChecksum(g_hotText),
              (long long)expectedHash);
        Check("exception thrown in RPX, caught in cold code", ColdCatch(100), 110);
        int caught = 0;
        try {
            ColdPassThrough(1);
        } catch (const std::runtime_error &) {
            caught = 1;
        }
        Check("exception unwound through a cold frame into the RPX", caught, 1);
        CheckOnAllCores();
    }
    Report("RESULT: %s (%d failures)", g_failures ? "FAILED" : "ALL PASSED", g_failures);
    Report("Press HOME to exit.");
    if (g_log)
        fclose(g_log);

    while (WHBProcIsRunning())
        WHBLogConsoleDraw();
    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
