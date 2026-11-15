#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <csetjmp>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>

static int tls_counter = 0; // thread_local unsupported by elf2rpl (TPREL relocs)
static std::atomic<uint32_t> a64{0};

int main() {
    WHBProcInit(); WHBLogConsoleInit();
    std::mutex m; std::condition_variable cv; bool done = false;
    std::thread t([&] { tls_counter = 5; a64.fetch_add(tls_counter); std::lock_guard<std::mutex> l(m); done = true; cv.notify_one(); });
    { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return done; }); }
    t.join();
    try { throw std::runtime_error("exc ok"); } catch (const std::exception& e) { WHBLogPrintf("%s", e.what()); }
    auto t0 = std::chrono::steady_clock::now();
    std::error_code ec; bool sd = std::filesystem::exists("fs:/vol/external01", ec);
    std::ofstream("fs:/vol/external01/wiicompiled_stdtest.txt") << "hello\n";
    WHBLogPrintf("tls main=%d a64=%llu sd=%d ns=%lld", tls_counter, (unsigned long long)a64.load(), (int)sd,
        (long long)std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0).count());
    WHBLogConsoleDraw();
    while (WHBProcIsRunning()) {}
    WHBLogConsoleFree(); WHBProcShutdown(); return 0;
}
