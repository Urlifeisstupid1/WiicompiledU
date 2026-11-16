// SPDX-License-Identifier: GPL-3.0-or-later
//
// 64-bit atomic operations for the Wii U (PORTING_NOTES.md, T2).
//
// 32-bit PowerPC has no 64-bit load-reserve/store-conditional, so GCC lowers
// std::atomic<uint64_t/int64_t/double> to the out-of-line __atomic_*_8 library
// calls; devkitPPC ships no libatomic, so they are provided here. Each address
// hashes to one of a few uninterruptible spin locks, which also exclude the
// other two cores. The memory-order arguments are ignored: every operation is
// sequentially consistent (the spin lock acquire/release are full barriers).
#include <coreinit/spinlock.h>

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define MKW_ATOMIC_LOCKS 16

static OSSpinLock s_locks[MKW_ATOMIC_LOCKS];
static volatile int s_locksReady;

static void InitLocks(void)
{
    for (int i = 0; i < MKW_ATOMIC_LOCKS; ++i)
        OSInitSpinLock(&s_locks[i]);
    s_locksReady = 1;
}

// Runs before ordinary static constructors; the lazy check in LockFor covers any
// 64-bit atomic used by an even earlier constructor (still single-threaded then).
__attribute__((constructor(101))) static void InitLocksEarly(void)
{
    if (!s_locksReady)
        InitLocks();
}

static OSSpinLock *LockFor(const volatile void *address)
{
    if (!s_locksReady)
        InitLocks();
    const uintptr_t key = (uintptr_t)address >> 3;
    return &s_locks[(key ^ (key >> 5)) & (MKW_ATOMIC_LOCKS - 1)];
}

#define LOCKED(address, body)                                  \
    do {                                                       \
        OSSpinLock *lock_ = LockFor(address);                  \
        OSUninterruptibleSpinLock_Acquire(lock_);              \
        body;                                                  \
        OSUninterruptibleSpinLock_Release(lock_);              \
    } while (0)

uint64_t __atomic_load_8(const volatile void *address, int order)
{
    (void)order;
    uint64_t value;
    LOCKED(address, value = *(const volatile uint64_t *)address);
    return value;
}

void __atomic_store_8(volatile void *address, uint64_t value, int order)
{
    (void)order;
    LOCKED(address, *(volatile uint64_t *)address = value);
}

uint64_t __atomic_exchange_8(volatile void *address, uint64_t value, int order)
{
    (void)order;
    uint64_t old;
    LOCKED(address, old = *(volatile uint64_t *)address; *(volatile uint64_t *)address = value);
    return old;
}

bool __atomic_compare_exchange_8(volatile void *address, void *expected, uint64_t desired,
                                 bool weak, int success, int failure)
{
    (void)weak;
    (void)success;
    (void)failure;
    bool done;
    LOCKED(address, {
        uint64_t current = *(volatile uint64_t *)address;
        uint64_t want;
        memcpy(&want, expected, sizeof(want));
        done = current == want;
        if (done)
            *(volatile uint64_t *)address = desired;
        else
            memcpy(expected, &current, sizeof(current));
    });
    return done;
}

#define FETCH_OP(name, expression)                                                  \
    uint64_t __atomic_fetch_##name##_8(volatile void *address, uint64_t v, int order) \
    {                                                                               \
        (void)order;                                                                \
        uint64_t old;                                                               \
        LOCKED(address, old = *(volatile uint64_t *)address;                        \
                        *(volatile uint64_t *)address = (expression));              \
        return old;                                                                 \
    }                                                                               \
    uint64_t __atomic_##name##_fetch_8(volatile void *address, uint64_t v, int order) \
    {                                                                               \
        (void)order;                                                                \
        uint64_t old, result;                                                       \
        LOCKED(address, old = *(volatile uint64_t *)address; result = (expression); \
                        *(volatile uint64_t *)address = result);                    \
        return result;                                                              \
    }

FETCH_OP(add, old + v)
FETCH_OP(sub, old - v)
FETCH_OP(and, old & v)
FETCH_OP(or, old | v)
FETCH_OP(xor, old ^ v)
FETCH_OP(nand, ~(old & v))

bool __atomic_is_lock_free(size_t size, const volatile void *address)
{
    (void)address;
    return size <= 4;
}
