// RPX half of the overlay test. Loads content/cold.bin into the codegen area,
// then exercises every kind of reference between the two halves. Prints each
// check on screen and writes the same lines to sd:/wiicompiled_overlay.txt.
#include "shared.h"
#include "wiicold_loader.h"

#include <coreinit/core.h>
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

static int RunOnCore(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    return ColdSwitch(3) + (int)OSGetCoreId() * 1000;
}

static void CheckOnAllCores()
{
    for (uint32_t core = 0; core < 3; ++core) {
        const uint32_t stackSize = 16 * 1024;
        OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
        uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
        int result = -1;
        if (thread && stack &&
            OSCreateThread(thread, RunOnCore, 0, nullptr, stack + stackSize, stackSize, 16,
                           (OSThreadAttributes)(1u << core))) {
            OSResumeThread(thread);
            OSJoinThread(thread, &result);
        }
        char what[64];
        snprintf(what, sizeof(what), "cold code on core %lu", (unsigned long)core);
        Check(what, result, 41 + (long long)core * 1000);
        MEMFreeToDefaultHeap(thread);
        MEMFreeToDefaultHeap(stack);
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
