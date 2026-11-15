// SPDX-License-Identifier: GPL-3.0-or-later
//
// wiicold_loader - see wiicold_loader.h and wiiu/tools/wiiu_cold_split.py.
#include "wiicold_loader.h"

#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/thread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Anchors: one object per RPX output section that cold code may reference. The
// split tool records their link addresses; their real addresses (after the Cafe
// loader moved the RPX) give the per-section delta. Names must match
// ANCHORS in wiiu_cold_split.py.
// Explicit sections keep each anchor out of the small-data sections, which wut.ld
// folds into a different output section than their plain counterparts.
__attribute__((used, noinline, section(".text.wiicold_anchor"))) void wiicold_anchor_text(void) {}
__attribute__((used, section(".rodata.wiicold_anchor"))) const uint32_t wiicold_anchor_rodata = 0x57434C44u;
__attribute__((used, section(".data.wiicold_anchor"))) uint32_t wiicold_anchor_data = 0x57434C44u;
__attribute__((used, section(".bss.wiicold_anchor"))) uint32_t wiicold_anchor_bss;

#define WIICOLD_ANCHOR_COUNT 4
#define R_PPC_ADDR32 1
#define R_PPC_ADDR16_LO 4
#define R_PPC_ADDR16_HI 5
#define R_PPC_ADDR16_HA 6

// From libgcc's unwinder: registers an extra .eh_frame region.
struct object_storage {
    void *words[16]; // larger than libgcc's struct object
};
extern void __register_frame_info(const void *begin, struct object_storage *object);
static struct object_storage s_frameObject;

typedef struct {
    char magic[4];
    uint32_t version;
    uint32_t linkBase;
    uint32_t imageSize;
    uint32_t codeSize;
    uint32_t ehFrameOffset;
    uint32_t ehFrameSize;
    uint32_t anchorCount;
    uint32_t relocCount;
} WiiColdHeader;

typedef struct {
    uint32_t offset;
    uint8_t type;
    uint8_t anchor;
    uint16_t reserved;
    uint32_t value;
} WiiColdReloc;

typedef struct {
    const WiiColdHeader *header;
    uint8_t *image;
    void *area;
    int result;
    char *error;
    size_t errorSize;
} CopyJob;

static void SetError(char *error, size_t errorSize, const char *format, ...)
{
    if (error == NULL || errorSize == 0)
        return;
    va_list args;
    va_start(args, format);
    vsnprintf(error, errorSize, format, args);
    va_end(args);
}

static int ApplyRelocs(const WiiColdHeader *header, const uint32_t *anchorLinks,
                       const WiiColdReloc *relocs, uint8_t *image, char *error, size_t errorSize)
{
    const uint32_t anchorReal[WIICOLD_ANCHOR_COUNT] = {
        (uint32_t)&wiicold_anchor_text,
        (uint32_t)&wiicold_anchor_rodata,
        (uint32_t)&wiicold_anchor_data,
        (uint32_t)&wiicold_anchor_bss,
    };
    uint32_t delta[WIICOLD_ANCHOR_COUNT];
    for (uint32_t i = 0; i < WIICOLD_ANCHOR_COUNT; ++i)
        delta[i] = anchorReal[i] - anchorLinks[i];

    for (uint32_t i = 0; i < header->relocCount; ++i) {
        const WiiColdReloc *r = &relocs[i];
        if (r->anchor >= WIICOLD_ANCHOR_COUNT || r->offset + 4 > header->imageSize) {
            SetError(error, errorSize, "cold image: bad relocation %lu", (unsigned long)i);
            return -1;
        }
        const uint32_t value = r->value + delta[r->anchor];
        uint8_t *where = image + r->offset;
        switch (r->type) {
        case R_PPC_ADDR32:
            memcpy(where, &value, 4);
            break;
        case R_PPC_ADDR16_LO: {
            const uint16_t half = (uint16_t)value;
            memcpy(where, &half, 2);
            break;
        }
        case R_PPC_ADDR16_HI: {
            const uint16_t half = (uint16_t)(value >> 16);
            memcpy(where, &half, 2);
            break;
        }
        case R_PPC_ADDR16_HA: {
            const uint16_t half = (uint16_t)((value + 0x8000u) >> 16);
            memcpy(where, &half, 2);
            break;
        }
        default:
            SetError(error, errorSize, "cold image: unsupported relocation type %u", (unsigned)r->type);
            return -1;
        }
    }
    return 0;
}

// Runs on the codegen core: only that core may switch the area's permissions.
static int CopyIntoCodegenArea(int argc, void *argv)
{
    (void)argc;
    CopyJob *job = (CopyJob *)argv;
    if (!OSSwitchSecCodeGenMode(CODEGEN_RW_)) {
        SetError(job->error, job->errorSize, "OSSwitchSecCodeGenMode(RW) failed");
        job->result = -1;
        return 0;
    }
    memcpy(job->area, job->image, job->header->imageSize);
    DCFlushRange(job->area, job->header->imageSize);
    if (!OSSwitchSecCodeGenMode(CODEGEN_R_X)) {
        SetError(job->error, job->errorSize, "OSSwitchSecCodeGenMode(RX) failed");
        job->result = -1;
        return 0;
    }
    ICInvalidateRange(job->area, job->header->imageSize);
    job->result = 0;
    return 0;
}

static int RunOnCodegenCore(CopyJob *job)
{
    const uint32_t core = OSGetCodegenCore() & 3u;
    if (OSGetCoreId() == core) {
        CopyIntoCodegenArea(0, job);
        return job->result;
    }
    const uint32_t stackSize = 16 * 1024;
    OSThread *thread = (OSThread *)MEMAllocFromDefaultHeapEx(sizeof(OSThread), 16);
    uint8_t *stack = (uint8_t *)MEMAllocFromDefaultHeapEx(stackSize, 16);
    int result = -1;
    if (thread && stack &&
        OSCreateThread(thread, CopyIntoCodegenArea, 0, (char *)job, stack + stackSize, stackSize, 16,
                       (OSThreadAttributes)(1u << core))) {
        OSResumeThread(thread);
        int unused = 0;
        OSJoinThread(thread, &unused);
        result = job->result;
    } else {
        SetError(job->error, job->errorSize, "could not start a thread on codegen core %lu",
                 (unsigned long)core);
    }
    if (thread)
        MEMFreeToDefaultHeap(thread);
    if (stack)
        MEMFreeToDefaultHeap(stack);
    return result;
}

int WiiColdLoad(const char *path, char *error, size_t errorSize)
{
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        SetError(error, errorSize, "cannot open %s", path);
        return -1;
    }
    fseek(file, 0, SEEK_END);
    const long fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    uint8_t *blob = (fileSize > (long)sizeof(WiiColdHeader))
        ? (uint8_t *)MEMAllocFromDefaultHeapEx((uint32_t)fileSize, 64) : NULL;
    if (blob == NULL || fread(blob, 1, (size_t)fileSize, file) != (size_t)fileSize) {
        fclose(file);
        if (blob)
            MEMFreeToDefaultHeap(blob);
        SetError(error, errorSize, "cannot read %s (%ld bytes)", path, fileSize);
        return -1;
    }
    fclose(file);

    int result = -1;
    const WiiColdHeader *header = (const WiiColdHeader *)blob;
    const uint32_t *anchorLinks = (const uint32_t *)(blob + sizeof(WiiColdHeader));
    const WiiColdReloc *relocs = (const WiiColdReloc *)(anchorLinks + header->anchorCount);
    uint8_t *image = (uint8_t *)(relocs + header->relocCount);
    void *area = NULL;
    uint32_t areaSize = 0;
    OSGetCodegenVirtAddrRange(&area, &areaSize);

    if (memcmp(header->magic, "WCLD", 4) != 0 || header->version != 1 ||
        header->anchorCount != WIICOLD_ANCHOR_COUNT) {
        SetError(error, errorSize, "%s is not a version-1 cold image", path);
    } else if ((size_t)(image - blob) + header->imageSize != (size_t)fileSize) {
        SetError(error, errorSize, "%s is truncated", path);
    } else if ((uint32_t)area != header->linkBase || header->imageSize > areaSize) {
        SetError(error, errorSize,
                 "codegen area is %p+0x%lX but the cold image needs 0x%08lX+0x%lX",
                 area, (unsigned long)areaSize, (unsigned long)header->linkBase,
                 (unsigned long)header->imageSize);
    } else if (ApplyRelocs(header, anchorLinks, relocs, image, error, errorSize) == 0) {
        DCFlushRange(image, header->imageSize);
        CopyJob job = {header, image, area, -1, error, errorSize};
        result = RunOnCodegenCore(&job);
        if (result == 0 && header->ehFrameSize > 4)
            __register_frame_info((uint8_t *)area + header->ehFrameOffset, &s_frameObject);
    }
    MEMFreeToDefaultHeap(blob);
    return result;
}
