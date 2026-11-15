// Codegen-area probe (round 2) for the WiiCompiled Wii U port.
//
// Round 1 on hardware: the area exists (0x01800000, 32 MiB, core 1) but
// OSCodegenCopy() failed immediately, even for a 48-byte stub. This round tries
// every route into the area, one step at a time, and appends each line to
// sd:/wiicompiled_codegen.txt immediately (flushed), so a crash still leaves a
// record of the last step reached.
//
//   A. Report OSGetCodegenMode() / OSGetSecCodeGenMode().
//   B. OSCodegenCopy() of a small stub at several sizes and offsets.
//   C. OSSwitchSecCodeGenMode(CODEGEN_RW_) + memcpy + OSSwitchSecCodeGenMode(CODEGEN_R_X),
//      then run the stub (codegen -> RPX callback, expect 141).
//   D. If C works: fill the WHOLE area with a nop sled ending in "li r3,7; blr"
//      and run it (expect 7), proving all 32 MiB are writable and executable.
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

static FILE *g_log;

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

// RPX-side function the codegen stub calls back into.
__attribute__((noinline)) int RpxCallback(int value)
{
    return value * 2 + 1;
}

typedef int (*IntFn)(int);

// Stub: saves LR, calls RpxCallback through an absolute address, adds 100.
// RpxCallback(20) = 41, so the stub returns 141.
static size_t BuildStub(uint32_t *code)
{
    const uint32_t target = (uint32_t)&RpxCallback;
    const uint32_t words[] = {
        0x7C0802A6u,                      // mflr  r0
        0x90010004u,                      // stw   r0,4(r1)
        0x9421FFF0u,                      // stwu  r1,-16(r1)
        0x3D800000u | (target >> 16),     // lis   r12,target@h
        0x618C0000u | (target & 0xFFFFu), // ori   r12,r12,target@l
        0x7D8903A6u,                      // mtctr r12
        0x4E800421u,                      // bctrl
        0x38630064u,                      // addi  r3,r3,100
        0x38210010u,                      // addi  r1,r1,16
        0x80010004u,                      // lwz   r0,4(r1)
        0x7C0803A6u,                      // mtlr  r0
        PPC_BLR,                          // blr
    };
    memcpy(code, words, sizeof(words));
    return sizeof(words);
}

static int RunCodegenTests(int argc, void *argv)
{
    (void)argc;
    (void)argv;

    void *area = NULL;
    uint32_t areaSize = 0;
    OSGetCodegenVirtAddrRange(&area, &areaSize);
    Report("codegen area: %p size 0x%08lX", area, (unsigned long)areaSize);
    Report("codegen core %lu, thread core %lu", (unsigned long)OSGetCodegenCore(),
           (unsigned long)OSGetCoreId());
    Report("A: OSGetCodegenMode=0x%08lX OSGetSecCodeGenMode=0x%08lX",
           (unsigned long)OSGetCodegenMode(), (unsigned long)OSGetSecCodeGenMode());
    Report("RPX: RunCodegenTests %p, RpxCallback %p", (void *)&RunCodegenTests, (void *)&RpxCallback);
    if (area == NULL || areaSize < 4096) {
        Report("RESULT: no codegen area");
        return 0;
    }

    uint32_t *buffer = (uint32_t *)MEMAllocFromDefaultHeapEx(areaSize, 64);
    if (buffer == NULL) {
        Report("RESULT: could not allocate %lu bytes", (unsigned long)areaSize);
        return 0;
    }
    const size_t stubSize = BuildStub(buffer);
    DCFlushRange(buffer, 4096);

    // B: OSCodegenCopy variants.
    static const uint32_t sizes[] = {4, 32, 48, 4096};
    static const uint32_t offsets[] = {0, 0x1000, 0x10000};
    int copyWorked = 0;
    for (unsigned o = 0; o < sizeof(offsets) / sizeof(offsets[0]); ++o) {
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
            BOOL ok = OSCodegenCopy((uint8_t *)area + offsets[o], buffer, sizes[i]);
            Report("B: OSCodegenCopy(off 0x%lX, size %lu) = %d", (unsigned long)offsets[o],
                   (unsigned long)sizes[i], (int)ok);
            copyWorked |= ok ? 1 : 0;
        }
    }

    // C: switch to RW, plain memcpy, switch to RX.
    BOOL rw = OSSwitchSecCodeGenMode(CODEGEN_RW_);
    Report("C: OSSwitchSecCodeGenMode(RW) = %d, mode now 0x%08lX", (int)rw,
           (unsigned long)OSGetSecCodeGenMode());
    int switchWorked = 0;
    if (rw) {
        memcpy(area, buffer, stubSize);
        Report("C: memcpy of stub into area done");
        DCFlushRange(area, stubSize);
        BOOL rx = OSSwitchSecCodeGenMode(CODEGEN_R_X);
        Report("C: OSSwitchSecCodeGenMode(RX) = %d", (int)rx);
        ICInvalidateRange(area, stubSize);
        if (rx) {
            int result = ((IntFn)area)(20);
            Report("C: codegen->RPX stub returned %d (expect 141)", result);
            switchWorked = (result == 141);
        }
    }

    // D: full-area sled via whichever route worked.
    if (switchWorked) {
        const uint32_t words = areaSize / 4u;
        for (uint32_t i = 0; i < words; ++i)
            buffer[i] = PPC_NOP;
        buffer[words - 2] = PPC_LI_R3(7);
        buffer[words - 1] = PPC_BLR;
        OSTime t0 = OSGetSystemTime();
        if (OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
            memcpy(area, buffer, areaSize);
            DCFlushRange(area, areaSize);
            OSSwitchSecCodeGenMode(CODEGEN_R_X);
            ICInvalidateRange(area, areaSize);
            OSTime t1 = OSGetSystemTime();
            Report("D: filled %lu MB in %lld ms", (unsigned long)(areaSize >> 20),
                   (long long)OSTicksToMilliseconds(t1 - t0));
            int sled = ((IntFn)area)(0);
            OSTime t2 = OSGetSystemTime();
            Report("D: full-area sled returned %d (expect 7) in %lld ms", sled,
                   (long long)OSTicksToMilliseconds(t2 - t1));
            Report("RESULT: %s", sled == 7 ? "codegen area usable for code (switch-mode route)"
                                           : "sled MISMATCH");
        } else {
            Report("RESULT: second RW switch failed");
        }
    } else if (copyWorked) {
        Report("RESULT: only some OSCodegenCopy calls worked, see B lines");
    } else {
        Report("RESULT: no route into the codegen area worked");
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
    g_log = fopen("fs:/vol/external01/wiicompiled_codegen.txt", "w");
    Report("WiiCompiled codegen probe, round 2");
    if (!g_log)
        Report("(could not open the SD log file)");

    // Codegen may only be used from its own core, so run the tests there.
    const uint32_t core = OSGetCodegenCore() & 3u;
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

    Report("Done. Press HOME to exit.");
    if (g_log) {
        fclose(g_log);
        g_log = NULL;
    }

    while (WHBProcIsRunning())
        WHBLogConsoleDraw();

    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
