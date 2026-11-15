// Phase 0 hello-world for the WiiCompiled Wii U port.
// Prints to TV and GamePad using wut's WHB console (OSScreen-backed),
// and runs a ProcUI loop so HOME menu / exit work under Aroma.
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <whb/proc.h>
#include <whb/log.h>
#include <whb/log_console.h>

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    WHBProcInit();
    WHBLogConsoleInit();

    WHBLogPrintf("WiiCompiled Wii U port - Phase 0");
    WHBLogPrintf("Hello from wut on Espresso!");
    WHBLogPrintf("Press HOME to exit.");
    WHBLogConsoleDraw();

    int frame = 0;
    while (WHBProcIsRunning()) {
        if ((frame++ % 60) == 0) {
            WHBLogPrintf("tick %d", frame / 60);
            WHBLogConsoleDraw();
        }
        OSSleepTicks(OSMillisecondsToTicks(16));
    }

    WHBLogConsoleFree();
    WHBProcShutdown();
    return 0;
}
