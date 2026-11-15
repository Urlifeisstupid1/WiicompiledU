// Cold half of the overlay test: linked into the codegen area.
#include "shared.h"

#include <stdexcept>
#include <string.h>

static int ColdHelper(int x)  // cold -> cold call (relative branch inside the image)
{
    return x * 7;
}

extern "C" int ColdCompute(int x)
{
    g_hotCounter += 1;                 // RPX small data
    g_hotArray[x & 63] = x;            // RPX bss
    return HotAdd(ColdHelper(x), g_hotArray[x & 63]) + g_hotCounter;
}

extern "C" int ColdSwitch(int x)
{
    switch (x) {
    case 0: return 11;
    case 1: return 23;
    case 2: return 37;
    case 3: return 41;
    case 4: return 59;
    case 5: return 67;
    case 6: return 73;
    default: return -1;
    }
}

extern "C" int ColdCatch(int code)
{
    try {
        HotThrow(code);
    } catch (const std::runtime_error &error) {
        return code + (int)strlen(error.what());
    }
    return -1;
}

extern "C" int ColdPassThrough(int code)
{
    HotThrow(code);
    return -1;
}

extern "C" uint32_t ColdChecksum(const char *text)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0, n = strlen(text); i < n; ++i)  // libc call from cold code
        hash = (hash ^ (uint8_t)text[i]) * 16777619u;
    return hash;
}
