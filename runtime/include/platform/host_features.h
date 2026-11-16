// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Compile-time host feature switches (PORTING_NOTES.md, D10).
//
// MKW_HAS_SDL: SDL3 (window, gamepads, audio streams, hints) is available. True on
// the PC/macOS builds; false on the Wii U, where aurora-wiiu and the Wii U platform
// files (src/platform/wiiu/) provide video, audio and input instead.
#if !defined(MKW_HAS_SDL)
#if defined(__WIIU__)
#define MKW_HAS_SDL 0
#else
#define MKW_HAS_SDL 1
#endif
#endif
