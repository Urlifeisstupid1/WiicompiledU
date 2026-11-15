// Loader-limit probe for the WiiCompiled Wii U port.
// The linker lays out .text as: main() | far_fn + BIGTEXT_MB blob | libwut.
// main()'s calls into libwut (WHBProcInit, WHBLogPrintf, ...) therefore cross
// the whole blob and go through linker-generated long-branch stubs.
// Expected screen output: the size line, "far_fn(4) = 13 (expect 13)", and
// HOME exits. If the loader rejects the RPX, the app fails to launch.
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>

#ifndef BIGTEXT_MB
#define BIGTEXT_MB 64
#endif

#define STR2(x) #x
#define STR(x) STR2(x)

__asm__(".section .text.bigtext_blob,\"ax\",@progbits\n"
        ".globl far_fn\n"
        ".type far_fn,@function\n"
        "far_fn:\n"
        "  mulli 3,3,3\n"
        "  addi 3,3,1\n"
        "  blr\n"
        ".space (" STR(BIGTEXT_MB) " * 1024 * 1024)\n"
        ".previous\n");

int far_fn(int x);

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBProcInit();
    WHBLogConsoleInit();

    WHBLogPrintf("WiiCompiled bigtext probe: %d MB of code", BIGTEXT_MB);
    WHBLogPrintf("far_fn(4) = %d (expect 13)", far_fn(4));
    WHBLogPrintf("Press HOME to exit.");
    WHBLogConsoleDraw();

    while (WHBProcIsRunning()) {
        WHBLogConsoleDraw();
    }

    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
