// Interface between the RPX ("hot") and codegen-area ("cold") halves of the
// overlay test.
#pragma once
#include <stdint.h>

// RPX side
extern "C" int HotAdd(int a, int b);
extern "C" void HotThrow(int code);
extern int g_hotCounter;       // .sdata (small, initialised)
extern int g_hotArray[64];     // .bss
extern const char g_hotText[]; // .rodata

// Cold side
extern "C" int ColdCompute(int x);      // calls back into the RPX, uses its data
extern "C" int ColdSwitch(int x);       // jump table
extern "C" int ColdCatch(int code);     // HotThrow through a cold frame, caught in cold code
extern "C" int ColdPassThrough(int code); // HotThrow escapes the cold frame, caught in the RPX
extern "C" uint32_t ColdChecksum(const char *text);
