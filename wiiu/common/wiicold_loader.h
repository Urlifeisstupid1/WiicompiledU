// SPDX-License-Identifier: GPL-3.0-or-later
//
// wiicold_loader - loads the "cold" code image produced by wiiu_cold_split.py
// into the Aroma codegen (JIT) area. See PORTING_NOTES.md (T4, D8).
#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Loads the image at `path` (normally "fs:/vol/content/cold.bin" inside the
// .wuhb), re-bases its references into the RPX, copies it into the codegen area
// on the codegen core and registers its unwind tables. Must be called before any
// cold code runs. Returns 0 on success; on failure returns non-zero and writes a
// readable reason into `error`.
int WiiColdLoad(const char *path, char *error, size_t errorSize);

#ifdef __cplusplus
}
#endif
