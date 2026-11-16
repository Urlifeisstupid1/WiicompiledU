// SPDX-License-Identifier: GPL-3.0-or-later
//
// Wii U implementation of the GuestFlat storage API (include/guest_flat_memory.h).
//
// The PC builds reserve a 4 GiB "flat" guest address space and intercept MMIO,
// deferred EFB reads and executable writes with host page protections. A 32-bit
// host cannot reserve 4 GiB, so on the Wii U every translated access already
// takes the checked page-table path (MKW_GUEST_FLAT_DISABLED, D1). What remains
// of GuestFlat here is its other job: owning the guest RAM backing store. As on
// PC, the physical/cached/uncached views of MEM1 (and of MEM2) alias one buffer,
// so a write through 0x80xxxxxx is visible at 0xC0xxxxxx and 0x00xxxxxx.
// Protections and fault interception are no-ops.
#include "guest_flat_memory.h"

#include "memory.h"

#include <coreinit/memdefaultheap.h>

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace GuestFlat {
namespace {

struct Mapping {
    uint32_t base = 0;
    uint64_t size = 0;
    uint8_t* host = nullptr;  // host address of `base`
};

std::vector<Mapping> g_mappings;
std::vector<uint8_t*> g_ownedBuffers;  // Backing::Owned regions, freed on re-initialisation
uint8_t* g_mem1 = nullptr;
uint8_t* g_mem2 = nullptr;
size_t g_mem1Size = 0;
size_t g_mem2Size = 0;
bool g_initialized = false;

uint8_t* AllocateZeroed(size_t size, const char* what)
{
    auto* memory = static_cast<uint8_t*>(MEMAllocFromDefaultHeapEx(static_cast<uint32_t>(size), 64));
    if (memory == nullptr) {
        throw std::runtime_error(std::string("Wii U: out of memory allocating ") + what + " (" +
                                 std::to_string(size >> 20) + " MiB)");
    }
    std::memset(memory, 0, size);
    return memory;
}

// Offset of a mirrored view inside its shared backing (MEM1/MEM2).
uint32_t ViewBase(uint32_t base, Backing backing)
{
    if (backing == Backing::Mem1) {
        if (base >= Memory::kMem1UncachedBase) return Memory::kMem1UncachedBase;
        if (base >= Memory::kMem1CachedBase) return Memory::kMem1CachedBase;
        return Memory::kMem1PhysicalBase;
    }
    if (base >= Memory::kMem2UncachedBase) return Memory::kMem2UncachedBase;
    if (base >= Memory::kMem2CachedBase) return Memory::kMem2CachedBase;
    return Memory::kMem2PhysicalBase;
}

} // namespace

bool IsActive()
{
    return g_initialized;
}

void Initialize(const std::vector<RegionRequest>& regions)
{
    // Size the shared backings to the largest mirror that uses them.
    size_t mem1Needed = 0;
    size_t mem2Needed = 0;
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        const uint64_t end = (region.base - ViewBase(region.base, region.backing)) + region.size;
        if (region.backing == Backing::Mem1) mem1Needed = std::max<size_t>(mem1Needed, end);
        if (region.backing == Backing::Mem2) mem2Needed = std::max<size_t>(mem2Needed, end);
    }
    if (mem1Needed > g_mem1Size) {
        if (g_mem1) MEMFreeToDefaultHeap(g_mem1);
        g_mem1 = AllocateZeroed(mem1Needed, "guest MEM1");
        g_mem1Size = mem1Needed;
    } else if (g_mem1) {
        std::memset(g_mem1, 0, g_mem1Size);
    }
    if (mem2Needed > g_mem2Size) {
        if (g_mem2) MEMFreeToDefaultHeap(g_mem2);
        g_mem2 = AllocateZeroed(mem2Needed, "guest MEM2");
        g_mem2Size = mem2Needed;
    } else if (g_mem2) {
        std::memset(g_mem2, 0, g_mem2Size);
    }

    for (uint8_t* buffer : g_ownedBuffers)
        MEMFreeToDefaultHeap(buffer);
    g_ownedBuffers.clear();
    g_mappings.clear();
    for (const auto& region : regions) {
        if (region.size == 0) continue;
        Mapping mapping{region.base, region.size, nullptr};
        if (region.backing == Backing::Mem1) {
            mapping.host = g_mem1 + (region.base - ViewBase(region.base, region.backing));
        } else if (region.backing == Backing::Mem2) {
            mapping.host = g_mem2 + (region.base - ViewBase(region.base, region.backing));
        } else {
            mapping.host = AllocateZeroed(static_cast<size_t>(region.size), "a guest region");
            g_ownedBuffers.push_back(mapping.host);
        }
        g_mappings.push_back(mapping);
    }
    g_initialized = true;
    std::printf("[wiiu] guest RAM: MEM1 %zu MiB, MEM2 %zu MiB, %zu mapped views\n", g_mem1Size >> 20,
                g_mem2Size >> 20, g_mappings.size());
}

uint8_t* HostPointer(uint32_t guestAddress)
{
    for (const auto& mapping : g_mappings) {
        if (guestAddress >= mapping.base && guestAddress - mapping.base < mapping.size)
            return mapping.host + (guestAddress - mapping.base);
    }
    return nullptr;
}

// No host page protections on the Wii U: deferred (EFB) reads and executable-write
// guards are enforced by the checked page-table path instead.
void ProtectDeferredRange(uint32_t, size_t) {}
void UnprotectDeferredRange(uint32_t, size_t) {}
void RegisterExecutableRange(uint32_t, uint32_t) {}

FaultCounters Counters()
{
    return {};
}

void LogFaultSummary() noexcept {}

bool HandleAccessViolation(void*, bool) noexcept
{
    return false;
}

} // namespace GuestFlat
