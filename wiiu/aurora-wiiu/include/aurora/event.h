// SPDX-License-Identifier: MIT
//
// Wii U replacement for aurora's include/aurora/event.h (aurora is MIT licensed;
// see THIRD-PARTY-NOTICES.md). Identical event types, but without SDL3: there is
// no SDL3 on the Wii U, so the `sdl` member is absent and controllers are
// identified by a plain integer. Runtime code that reads `event->sdl` is guarded
// by MKW_HAS_SDL. This directory is searched before aurora's own include
// directory in the Wii U build (PORTING_NOTES.md, D10).
#ifndef AURORA_EVENT_H
#define AURORA_EVENT_H

#include <aurora/aurora.h>

#ifdef __cplusplus
#include <cstdint>

extern "C" {
#else
#include "stdint.h"
#endif

typedef enum {
  AURORA_NONE,
  AURORA_EXIT,
  AURORA_SDL_EVENT,
  AURORA_WINDOW_MOVED,
  AURORA_WINDOW_RESIZED,
  AURORA_CONTROLLER_ADDED,
  AURORA_CONTROLLER_REMOVED,
  AURORA_PAUSED,
  AURORA_UNPAUSED,
  AURORA_DISPLAY_SCALE_CHANGED,
} AuroraEventType;

typedef uint32_t AuroraControllerId;

struct AuroraEvent {
  AuroraEventType type;
  union {
    AuroraWindowPos windowPos;
    AuroraWindowSize windowSize;
    AuroraControllerId controller;
  };
};

#ifdef __cplusplus
}
#endif

#endif
