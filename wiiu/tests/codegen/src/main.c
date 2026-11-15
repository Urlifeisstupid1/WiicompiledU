// Codegen-area probe for the WiiCompiled Wii U port.
//
// 1. Prints the codegen (JIT) area that Aroma grants homebrew, the codegen core,
//    and where the RPX code sits, to test the theory that the ~28 MB RPX limit
//    is "64 MiB max_codesize minus a 32 MiB codegen area".
// 2. Fills the WHOLE codegen area with nops ending in "li r3,7; blr", copies it
//    in with OSCodegenCopy and calls the start: returning 7 proves every byte of
//    the area is writable and executable.
// 3. Places a stub in the area that calls back into an RPX function through an
//    absolute address (lis/ori/mtctr/bctrl), proving codegen code and RPX code
//    can call each other - which translated code split across both needs.
//
// All results are printed on screen and written to sd:/wiicompiled_codegen.txt.
#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

#define PPC_NOP 0x60000000u
#define PPC_BLR 0x4E800020u
#define PPC_LI_R3(v) (0x38600000u | ((uint32_t)(v) & 0xFFFFu))

static char g_report[2048];
static size_t g_reportLength;

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
    size_t length = strlen(line);
    if (g_reportLength + length + 2 < sizeof(g_report)) {
        memcpy(g_report + g_reportLength, line, length);
        g_reportLength += length;
        g_report[g_reportLength++] = '\n';
        g_report[g_reportLength] = '\0';
    }
}

// RPX-side function the codegen stub calls back into.
__attribute__((noinline)) int RpxCallback(int value)
{
    return value * 2 + 1;
}

typedef int (*IntFn)(int);

static int RunCodegenTests(int argc, void *argv)
{
    (void)argc;
    (void)argv;

    void *area = NULL;
    uint32_t areaSize = 0;
    OSGetCodegenVirtAddrRange(&area, &areaSize);
    Report("codegen area: %p size 0x%08lX (%lu MB)", area, (unsigned long)areaSize,
           (unsigned long)(areaSize >> 20));
    Report("codegen core: %lu, this thread on core %lu", (unsigned long)OSGetCodegenCore(),
           (unsigned long)OSGetCoreId());
    Report("RPX code: main-ish at %p, RpxCallback at %p", (void *)&RunCodegenTests, (void *)&RpxCallback);
    if (area == NULL || areaSize < 64) {
        Report("RESULT: no codegen area available");
        return 0;
    }

    // Test 2: whole-area nop sled.
    uint32_t words = areaSize / 4u;
    uint32_t *buffer = (uint32_t *)MEMAllocFromDefaultHeapEx(areaSize, 64);
    if (buffer == NULL) {
        Report("RESULT: could not allocate %lu bytes for the sled", (unsigned long)areaSize);
        return 0;
    }
    for (uint32_t i = 0; i < words; ++i)
        buffer[i] = PPC_NOP;
    buffer[words - 2] = PPC_LI_R3(7);
    buffer[words - 1] = PPC_BLR;
    DCFlushRange(buffer, areaSize);
    OSTime t0 = OSGetSystemTime();
    BOOL copied = OSCodegenCopy(area, buffer, areaSize);
    OSTime t1 = OSGetSystemTime();
    Report("OSCodegenCopy(full area): %s in %lld us", copied ? "OK" : "FAILED",
           (long long)OSTicksToMicroseconds(t1 - t0));
    if (copied) {
        ICInvalidateRange(area, areaSize);
        int sled = ((IntFn)area)(0);
        OSTime t2 = OSGetSystemTime();
        Report("full-area sled returned %d (expect 7) in %lld us", sled,
               (long long)OSTicksToMicroseconds(t2 - t1));
    }

    // Test 3: stub in the area that calls back into the RPX.
    uint32_t target = (uint32_t)&RpxCallback;
    uint32_t code[] = {
        0x7C0802A6u,                                   // mflr  r0
        0x90010004u,                                   // stw   r0,4(r1)
        0x9421FFF0u,                                   // stwu  r1,-16(r1)
        0x3D800000u | (target >> 16),                  // lis   r12,target@h
        0x618C0000u | (target & 0xFFFFu),              // ori   r12,r12,target@l
        0x7D8903A6u,                                   // mtctr r12
        0x4E800421u,                                   // bctrl
        0x38630064u,                                   // addi  r3,r3,100
        0x38210010u,                                   // addi  r1,r1,16
        0x80010004u,                                   // lwz   r0,4(r1)
        0x7C0803A6u,                                   // mtlr  r0
        PPC_BLR,                                       // blr
    };
    memcpy(buffer, code, sizeof(code));
    DCFlushRange(buffer, sizeof(code));
    if (OSCodegenCopy(area, buffer, sizeof(code))) {
        ICInvalidateRange(area, sizeof(code));
        int result = ((IntFn)area)(20);
        Report("codegen->RPX call returned %d (expect 141)", result);
        Report("RESULT: %s", (result == 141) ? "codegen area usable for code" : "callback MISMATCH");
    } else {
        Report("RESULT: OSCodegenCopy for the stub FAILED");
    }
    MEMFreeToDefaultHeap(buffer);
    return 0;
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();
    Report("WiiCompiled codegen probe");

    // Codegen may only be switched on its own core, so run the tests there.
    uint32_t core = OSGetCodegenCore() & 3u;
    OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
    const uint32_t stackSize = 64 * 1024;
    uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
    if (thread && stack &&
        OSCreateThread(thread, RunCodegenTests, 0, NULL, stack + stackSize, stackSize, 16,
                       (OSThreadAttributes)(1u << core))) {
        OSResumeThread(thread);
        int unused = 0;
        OSJoinThread(thread, &unused);
    } else {
        Report("could not start a thread on core %lu", (unsigned long)core);
    }

    FILE *file = fopen("fs:/vol/external01/wiicompiled_codegen.txt", "w");
    if (file) {
        fputs(g_report, file);
        fclose(file);
        Report("report written to SD: wiicompiled_codegen.txt");
    } else {
        Report("could not write the SD report");
    }
    Report("Press HOME to exit.");

    while (WHBProcIsRunning())
        WHBLogConsoleDraw();

    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
