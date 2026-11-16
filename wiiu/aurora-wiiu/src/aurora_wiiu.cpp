// SPDX-License-Identifier: GPL-3.0-or-later
//
// aurora-wiiu, Phase 2 (headless): the aurora entry points whose return value or
// side effect matters. The no-op rest is generated into aurora_wiiu_stubs.cpp by
// wiiu/tools/gen_aurora_stubs.py. Phase 3 replaces the GX side with GX2, Phase 4
// replaces PAD with GamePad / Pro Controller / Wiimote input. See
// PORTING_NOTES.md (D10).
//
// GXGetTexBufferSize is ported from aurora's lib/dolphin/gx/GXTexture.cpp
// (MIT License, Copyright (c) 2022 Luke Street; see THIRD-PARTY-NOTICES.md).

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx/GXEnum.h>
#include <dolphin/gx/GXFifo.h>
#include <dolphin/gx/GXManage.h>
#include <dolphin/gx/GXTexture.h>
#include <dolphin/gx/GXVert.h>
#include <dolphin/pad.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

// ---- lifecycle -------------------------------------------------------------------

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config)
{
    (void)argc;
    (void)argv;
    AuroraInfo info{};
    info.backend = BACKEND_AUTO;
    info.userPath = config ? config->userPath : nullptr;
    info.cachePath = config ? config->cachePath : nullptr;
    info.window = nullptr;
    const uint32_t width = (config && config->windowWidth) ? config->windowWidth : 854;
    const uint32_t height = (config && config->windowHeight) ? config->windowHeight : 480;
    info.windowSize = AuroraWindowSize{width, height, 640, 528, width, height, 1.0f};
    info.initializationStatus = AURORA_INITIALIZATION_SUCCESS;
    info.initializationError = nullptr;
    std::printf("[aurora-wiiu] headless Phase 2 backend: no rendering, no audio, no input\n");
    return info;
}

const AuroraEvent* aurora_update()
{
    // No window, no controllers: an empty, AURORA_NONE-terminated event list.
    static AuroraEvent none{};
    none.type = AURORA_NONE;
    return &none;
}

bool aurora_begin_frame()
{
    return true;
}

bool aurora_begin_frame_gx()
{
    return true;
}

bool aurora_begin_frame_window()
{
    return true;
}

AuroraImguiFrame* aurora_end_frame_window()
{
    return nullptr;  // no ImGui on the Wii U
}

AuroraDisplayMode aurora_get_display_mode()
{
    return AURORA_DISPLAY_MODE_EXCLUSIVE;
}

// ---- GX: FIFO objects and pure computations ----------------------------------------

namespace {
GXFifoObj g_cpuFifo{};
}

GXFifoObj* GXInit(void* base, u32 size)
{
    (void)base;
    (void)size;
    return &g_cpuFifo;
}

GXFifoObj* GXGetCPUFifo(void)
{
    return &g_cpuFifo;
}

void GXEnd(void) {}

extern "C" void __GXSetSUTexRegs() {}

namespace aurora::gx::fifo {
bool submit_raw_draw(GXPrimitive prim, GXVtxFmt fmt, const uint8_t* vertices, uint16_t vtxCount,
                     uint32_t vertexBytes)
{
    (void)prim;
    (void)fmt;
    (void)vertices;
    (void)vtxCount;
    (void)vertexBytes;
    return true;  // consumed (nothing is drawn in Phase 2)
}
} // namespace aurora::gx::fifo

u32 GXGetTexBufferSize(u16 width, u16 height, u32 fmt, GXBool mips, u8 maxLod)
{
    if (fmt == GX_TF_R8_PC || fmt == GX_TF_RGBA8_PC) {
        const u32 bytesPerPixel = fmt == GX_TF_R8_PC ? 1u : 4u;
        const u32 mipCount = mips ? static_cast<u32>(maxLod) + 1u : 1u;
        uint64_t byteCount = 0;
        for (u32 mip = 0; mip < mipCount; ++mip) {
            byteCount += static_cast<uint64_t>(width) * height * bytesPerPixel;
            if (width == 1 && height == 1)
                break;
            width = std::max<u16>(width / 2, 1);
            height = std::max<u16>(height / 2, 1);
        }
        return byteCount > UINT32_MAX ? UINT32_MAX : static_cast<u32>(byteCount);
    }

    int32_t shiftX = 0;
    int32_t shiftY = 0;
    switch (fmt) {
    case GX_TF_I4:
    case GX_TF_C4:
    case GX_TF_CMPR:
    case GX_CTF_R4:
    case GX_CTF_Z4:
        shiftX = 3;
        shiftY = 3;
        break;
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_C8:
    case GX_TF_Z8:
    case GX_CTF_RA4:
    case GX_CTF_A8:
    case GX_CTF_R8:
    case GX_CTF_G8:
    case GX_CTF_B8:
    case GX_CTF_Z8M:
    case GX_CTF_Z8L:
        shiftX = 3;
        shiftY = 2;
        break;
    case GX_TF_IA8:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_RGBA8:
    case GX_TF_C14X2:
    case GX_TF_Z16:
    case GX_TF_Z24X8:
    case GX_CTF_RA8:
    case GX_CTF_RG8:
    case GX_CTF_GB8:
    case GX_CTF_Z16L:
        shiftX = 2;
        shiftY = 2;
        break;
    default:
        break;
    }
    const u32 bitSize = fmt == GX_TF_RGBA8 || fmt == GX_TF_Z24X8 ? 64 : 32;
    u32 bufLen = 0;
    if (mips) {
        const u32 mipCount = static_cast<u32>(maxLod) + 1;
        for (u32 mip = 0; mip < mipCount; ++mip) {
            const u32 tileX = ((width + (1 << shiftX) - 1) >> shiftX);
            const u32 tileY = ((height + (1 << shiftY) - 1) >> shiftY);
            bufLen += bitSize * tileX * tileY;
            if (width == 1 && height == 1)
                return bufLen;
            width = (width < 2) ? 1 : width / 2;
            height = (height < 2) ? 1 : height / 2;
        }
    } else {
        const u32 tileX = ((width + (1 << shiftX) - 1) >> shiftX);
        const u32 tileY = ((height + (1 << shiftY) - 1) >> shiftY);
        bufLen = bitSize * tileX * tileY;
    }
    return bufLen;
}

// ---- PAD: no controllers yet (Phase 4) ----------------------------------------------

BOOL PADInit()
{
    return TRUE;
}

u32 PADRead(PADStatus* status)
{
    if (status != nullptr) {
        for (int port = 0; port < PAD_CHANMAX; ++port) {
            status[port] = PADStatus{};
            status[port].err = PAD_ERR_NO_CONTROLLER;
        }
    }
    return 0;
}

BOOL PADReset(u32 mask)
{
    (void)mask;
    return TRUE;
}

BOOL PADRecalibrate(u32 mask)
{
    (void)mask;
    return TRUE;
}
