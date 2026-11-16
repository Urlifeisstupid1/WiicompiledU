// Runtime-primitives test for the WiiCompiled Wii U port. Results go to the
// screen and to sd:/wiicompiled_primitives.txt (flushed line by line).
//   1. MKW_THREAD_LOCAL: int, std::string and array thread-locals on all 3 cores.
//   2. 64-bit atomics (atomic64.c shim) under contention from all 3 cores.
//   3. HostContext fibers (runtime/src/host_context.cpp on libco ppc.c) on the
//      codegen core, switching thousands of times, calling cold-overlay code and
//      keeping floating-point and integer state across switches.
#include "host_context.h"
#include "platform/mkw_thread_local.h"
#include "wiicold_loader.h"

#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/log.h>
#include <whb/log_console.h>
#include <whb/proc.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <string>

extern "C" double ColdStep(double accumulator, int step);

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

static void Check(const char *what, bool ok, const char *detail)
{
    if (!ok)
        ++g_failures;
    Report("%s %s%s%s", ok ? "OK  " : "FAIL", what, detail[0] ? ": " : "", detail);
}

// Runs `entry(argument)` on a new thread pinned to `core` and waits for it.
static int RunOnCore(uint32_t core, OSThreadEntryPointFn entry, void *argument)
{
    const uint32_t stackSize = 256 * 1024;
    OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
    uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
    int result = -1;
    if (thread && stack &&
        OSCreateThread(thread, entry, 0, (char *)argument, stack + stackSize, stackSize, 16,
                       (OSThreadAttributes)(1u << core))) {
        OSResumeThread(thread);
        OSJoinThread(thread, &result);
    }
    MEMFreeToDefaultHeap(thread);
    MEMFreeToDefaultHeap(stack);
    return result;
}

// ---- 1. MKW_THREAD_LOCAL ----------------------------------------------------
static MKW_THREAD_LOCAL(int) t_value = 7;
static MKW_THREAD_LOCAL(std::string) t_name = std::string("initial");
using Table = int[16];
static MKW_THREAD_LOCAL(Table) t_table{};

static int TlsWorker(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    const int core = (int)OSGetCoreId();
    int ok = (t_value == 7 && MKW_TLS(t_name) == "initial" && t_table[3] == 0) ? 1 : 0;
    t_value = 100 + core;
    t_value += 1;
    ++t_value;
    MKW_TLS(t_name) = "core" + std::to_string(core);
    for (int i = 0; i < 16; ++i)
        t_table[i] = core * 16 + i;
    OSSleepTicks(OSMillisecondsToTicks(50));  // let the other threads write theirs
    ok &= (t_value == 102 + core) ? 1 : 0;
    ok &= (MKW_TLS(t_name) == "core" + std::to_string(core)) ? 1 : 0;
    for (int i = 0; i < 16; ++i)
        ok &= (t_table[i] == core * 16 + i) ? 1 : 0;
    return ok;
}

struct TlsRun {
    OSThread thread;
    uint8_t *stack;
    uint32_t core;
};

static void TestThreadLocal()
{
    // Three threads at once, one per core, so their writes interleave.
    TlsRun runs[3];
    const uint32_t stackSize = 64 * 1024;
    for (uint32_t core = 0; core < 3; ++core) {
        runs[core].core = core;
        runs[core].stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
        OSCreateThread(&runs[core].thread, TlsWorker, 0, nullptr, runs[core].stack + stackSize, stackSize,
                       16, (OSThreadAttributes)(1u << core));
    }
    for (auto &run : runs)
        OSResumeThread(&run.thread);
    for (auto &run : runs) {
        int ok = 0;
        OSJoinThread(&run.thread, &ok);
        char what[64];
        snprintf(what, sizeof(what), "MKW_THREAD_LOCAL isolated on core %lu", (unsigned long)run.core);
        Check(what, ok == 1, "");
        MEMFreeToDefaultHeap(run.stack);
    }
    char detail[96];
    snprintf(detail, sizeof(detail), "t_value=%d name=%s table[3]=%d", (int)t_value, MKW_TLS(t_name).c_str(),
             t_table[3]);
    Check("main thread's copies untouched", t_value == 7 && MKW_TLS(t_name) == "initial" && t_table[3] == 0,
          detail);
}

// ---- 2. 64-bit atomics --------------------------------------------------------
static std::atomic<uint64_t> g_counter64{0};
static std::atomic<double> g_counterDouble{0.0};
constexpr int kAtomicIterations = 100000;
constexpr int kDoubleIterations = 20000;

static int AtomicWorker(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    for (int i = 0; i < kAtomicIterations; ++i)
        g_counter64.fetch_add(0x100000001ull, std::memory_order_relaxed);  // both halves
    for (int i = 0; i < kDoubleIterations; ++i) {
        double expected = g_counterDouble.load();
        while (!g_counterDouble.compare_exchange_weak(expected, expected + 1.0)) {
        }
    }
    return 0;
}

static void TestAtomics()
{
    TlsRun runs[3];
    const uint32_t stackSize = 64 * 1024;
    OSTime start = OSGetSystemTime();
    for (uint32_t core = 0; core < 3; ++core) {
        runs[core].stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
        OSCreateThread(&runs[core].thread, AtomicWorker, 0, nullptr, runs[core].stack + stackSize, stackSize,
                       16, (OSThreadAttributes)(1u << core));
    }
    for (auto &run : runs)
        OSResumeThread(&run.thread);
    for (auto &run : runs) {
        int unused = 0;
        OSJoinThread(&run.thread, &unused);
        MEMFreeToDefaultHeap(run.stack);
    }
    const long long ms = (long long)OSTicksToMilliseconds(OSGetSystemTime() - start);
    const uint64_t expected64 = 3ull * kAtomicIterations * 0x100000001ull;
    char detail[128];
    snprintf(detail, sizeof(detail), "0x%016llX (expected 0x%016llX), %lld ms total",
             (unsigned long long)g_counter64.load(), (unsigned long long)expected64, ms);
    Check("std::atomic<uint64_t> fetch_add from 3 cores", g_counter64.load() == expected64, detail);
    snprintf(detail, sizeof(detail), "%.1f (expected %.1f)", g_counterDouble.load(), 3.0 * kDoubleIterations);
    Check("std::atomic<double> CAS loop from 3 cores", g_counterDouble.load() == 3.0 * kDoubleIterations, detail);
}

// ---- 3. HostContext fibers on the codegen core ---------------------------------
constexpr int kFibers = 3;
constexpr int kRounds = 5000;

struct FiberState {
    HostContext::Handle self = nullptr;
    HostContext::Handle next = nullptr;
    HostContext::Handle scheduler = nullptr;
    int index = 0;
    int steps = 0;
    double accumulator = 0.0;
    double reference = 0.0;
    bool done = false;
};

static FiberState g_fibers[kFibers];
static HostContext::Handle g_scheduler;
static int g_isCurrentFailures;

static void FiberEntry(void *argument)
{
    FiberState *state = static_cast<FiberState *>(argument);
    // Locals that live across switches: kept in callee-saved GPRs/FPRs by GCC.
    double local = 1.0 + state->index;
    long long localInt = 0;
    for (int round = 0; round < kRounds; ++round) {
        if (!HostContext::IsCurrent(state->self))
            ++g_isCurrentFailures;
        local = ColdStep(local, round + state->index);
        localInt += round * (state->index + 1);
        ++state->steps;
        HostContext::Switch(state->next);
    }
    state->accumulator = local + static_cast<double>(localInt);
    state->done = true;
    HostContext::Switch(state->scheduler);
    for (;;)  // never resumed; a fiber must not return
        HostContext::Switch(state->scheduler);
}

static int FiberSchedulerThread(int argc, void *argv)
{
    (void)argc;
    (void)argv;
    if (!HostContext::InitializeScheduler(&g_scheduler))
        return -1;
    for (int i = 0; i < kFibers; ++i) {
        g_fibers[i].index = i;
        g_fibers[i].scheduler = g_scheduler;
        g_fibers[i].self = HostContext::Create(64 * 1024, FiberEntry, &g_fibers[i]);
        if (!g_fibers[i].self)
            return -2;
    }
    for (int i = 0; i < kFibers; ++i)
        g_fibers[i].next = (i + 1 < kFibers) ? g_fibers[i + 1].self : g_scheduler;
    // Each pass runs fiber 0 -> 1 -> 2 -> back here, once per round.
    double schedulerLocal = 3.25;
    for (int round = 0; round < kRounds + 1; ++round) {
        HostContext::Switch(g_fibers[0].self);
        schedulerLocal = schedulerLocal * 1.0000001;
    }
    for (int i = 0; i < kFibers; ++i)
        HostContext::Destroy(g_fibers[i].self);
    HostContext::ShutdownScheduler(g_scheduler);
    return schedulerLocal > 3.25 ? 1 : 0;
}

static void TestFibers()
{
    // Reference values computed without fibers.
    for (int i = 0; i < kFibers; ++i) {
        double local = 1.0 + i;
        long long localInt = 0;
        for (int round = 0; round < kRounds; ++round) {
            local = local * 0.5 + static_cast<double>(round + i) * 0.25;
            localInt += round * (i + 1);
        }
        g_fibers[i].reference = local + static_cast<double>(localInt);
    }
    OSTime start = OSGetSystemTime();
    const uint32_t core = OSGetCodegenCore() & 3u;
    const int result = RunOnCore(core, FiberSchedulerThread, nullptr);
    const long long us = (long long)OSTicksToMicroseconds(OSGetSystemTime() - start);
    char detail[160];
    snprintf(detail, sizeof(detail), "scheduler result %d, %d x %d switches in %lld us", result, kFibers + 1,
             kRounds, us);
    Check("HostContext fibers ran on the codegen core", result == 1, detail);
    for (int i = 0; i < kFibers; ++i) {
        snprintf(detail, sizeof(detail), "steps %d/%d, value %.6f (expected %.6f)", g_fibers[i].steps, kRounds,
                 g_fibers[i].accumulator, g_fibers[i].reference);
        char what[64];
        snprintf(what, sizeof(what), "fiber %d state survived switches", i);
        Check(what, g_fibers[i].done && g_fibers[i].steps == kRounds &&
                        g_fibers[i].accumulator == g_fibers[i].reference, detail);
    }
    snprintf(detail, sizeof(detail), "%d mismatches", g_isCurrentFailures);
    Check("HostContext::IsCurrent inside fibers", g_isCurrentFailures == 0, detail);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    WHBProcInit();
    WHBLogConsoleInit();
    g_log = fopen("fs:/vol/external01/wiicompiled_primitives.txt", "w");
    Report("WiiCompiled runtime-primitives test");

    char error[160] = "";
    if (WiiColdLoad("fs:/vol/content/cold.bin", error, sizeof(error)) != 0) {
        Check("cold image loaded", false, error);
    } else {
        Check("cold image loaded", true, "");
        Report("... 1/3 MKW_THREAD_LOCAL");
        TestThreadLocal();
        Report("... 2/3 64-bit atomics");
        TestAtomics();
        Report("... 3/3 HostContext fibers");
        TestFibers();
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
