// SPDX-License-Identifier: GPL-3.0-or-later
//
// Per-thread storage behind MKW_THREAD_LOCAL on the Wii U (see
// include/platform/mkw_thread_local.h).
//
// Every variable gets a fixed slot (offset) in one arena layout. Each OS thread
// lazily allocates its own arena, found through OS thread-specific value
// kSpecificId, and initialises a slot from the variable's prototype the first
// time that thread touches it.
#include "platform/mkw_thread_local.h"

#include <coreinit/debug.h>
#include <coreinit/memdefaultheap.h>
#include <coreinit/spinlock.h>
#include <coreinit/thread.h>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace MkwTls {
namespace {

constexpr OSThreadSpecificID kSpecificId = OS_THREAD_SPECIFIC_13;
constexpr uint32_t kMaxSlots = 256;
constexpr size_t kArenaSize = 64 * 1024;

struct SlotInfo {
    size_t offset;
    CopyFn copy;
    const void* prototype;
};

struct Arena {
    uint8_t initialized[kMaxSlots];
    alignas(16) uint8_t data[kArenaSize];
};

SlotInfo g_slots[kMaxSlots];
std::atomic<uint32_t> g_slotCount{0};
size_t g_nextOffset = 0;
OSSpinLock g_registerLock;
std::atomic<bool> g_lockReady{false};

void LockRegistry()
{
    // Registration runs during static initialisation, possibly before any other
    // code; initialise the spin lock on first use.
    bool expected = false;
    if (!g_lockReady.load(std::memory_order_acquire) &&
        g_lockReady.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
        OSInitSpinLock(&g_registerLock);
    }
    OSUninterruptibleSpinLock_Acquire(&g_registerLock);
}

[[noreturn]] void Fail(const char* reason)
{
    OSFatal(reason);
    std::abort();
}

Arena* CurrentArena()
{
    auto* arena = static_cast<Arena*>(OSGetThreadSpecific(kSpecificId));
    if (arena != nullptr)
        return arena;
    arena = static_cast<Arena*>(MEMAllocFromDefaultHeapEx(sizeof(Arena), 64));
    if (arena == nullptr)
        Fail("MKW_THREAD_LOCAL: out of memory for a thread's storage");
    std::memset(arena, 0, sizeof(Arena));
    OSSetThreadSpecific(kSpecificId, arena);
    return arena;
}

} // namespace

uint32_t Register(size_t size, size_t alignment, CopyFn copy, const void* prototype)
{
    LockRegistry();
    const uint32_t index = g_slotCount.load(std::memory_order_relaxed);
    const size_t offset = (g_nextOffset + alignment - 1) & ~(alignment - 1);
    if (index >= kMaxSlots || alignment > 16 || offset + size > kArenaSize) {
        OSUninterruptibleSpinLock_Release(&g_registerLock);
        Fail("MKW_THREAD_LOCAL: too many or too large thread-local variables (raise kMaxSlots/kArenaSize)");
    }
    g_slots[index] = SlotInfo{offset, copy, prototype};
    g_nextOffset = offset + size;
    g_slotCount.store(index + 1, std::memory_order_release);
    OSUninterruptibleSpinLock_Release(&g_registerLock);
    return index;
}

void* Slot(uint32_t index)
{
    Arena* arena = CurrentArena();
    const SlotInfo& slot = g_slots[index];
    void* storage = arena->data + slot.offset;
    if (!arena->initialized[index]) [[unlikely]] {
        slot.copy(storage, slot.prototype);
        arena->initialized[index] = 1;
    }
    return storage;
}

} // namespace MkwTls
