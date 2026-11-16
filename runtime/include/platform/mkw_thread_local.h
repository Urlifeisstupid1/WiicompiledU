// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
// Portable spelling of `thread_local` for the runtime.
//
//   MKW_THREAD_LOCAL(T) name = init;          // instead of: thread_local T name = init;
//   inline MKW_THREAD_LOCAL(T) name{};        // instead of: inline thread_local T name{};
//   extern MKW_THREAD_LOCAL(T) name;          // instead of: extern thread_local T name;
//   MKW_TLS_CONSTINIT                         // instead of: constinit (on thread-locals only)
//   MKW_TLS(name).member(...)                 // member access on a thread-local object
//
// Arrays need a type alias, so the macro argument is a single type:
//   using MemoTable = Entry[512];
//   inline MKW_THREAD_LOCAL(MemoTable) g_memo{};
//
// Everywhere except the Wii U this is exactly `thread_local T`.
//
// On the Wii U (Cafe OS + wut), compiler TLS cannot be used: GCC emits local-exec
// TLS (R_PPC_TPREL16_*) for executables, which elf2rpl rejects, and the local-exec
// thread pointer (r2) is the SDA2 base on Cafe OS. MKW_THREAD_LOCAL(T) is then
// MkwThreadLocal<T>: every OS thread gets a lazily allocated storage block (found
// through OSGetThreadSpecific), each variable owns a fixed slot in it, and a slot
// is copy-initialised from the variable's initial value the first time a thread
// touches it. The wrapper forwards the usual operators so most uses compile
// unchanged; anything else can call `.get()`. Per-thread copies are never
// destroyed (host threads are few and long-lived). See PORTING_NOTES.md, D2/T1.

#if !defined(__WIIU__)

#define MKW_THREAD_LOCAL(...) thread_local __VA_ARGS__
#define MKW_TLS_CONSTINIT constinit
#define MKW_TLS(name) (name)

#else

#include <cstddef>
#include <cstdint>
#include <new>
#include <type_traits>
#include <utility>

#define MKW_THREAD_LOCAL(...) ::MkwThreadLocal<__VA_ARGS__>
#define MKW_TLS_CONSTINIT
#define MKW_TLS(name) ((name).get())

namespace MkwTls {

using CopyFn = void (*)(void* destination, const void* prototype);

// Registers a variable's slot; returns its index. Thread-safe.
uint32_t Register(size_t size, size_t alignment, CopyFn copy, const void* prototype);
// The calling thread's storage for slot `index`, initialised on first use.
void* Slot(uint32_t index);

template <typename T>
void CopyConstruct(void* destination, const void* prototype)
{
    if constexpr (std::is_array_v<T>) {
        using Element = std::remove_extent_t<T>;
        constexpr size_t count = sizeof(T) / sizeof(Element);
        auto* out = static_cast<Element*>(destination);
        const auto* in = static_cast<const Element*>(prototype);
        for (size_t i = 0; i < count; ++i)
            ::new (static_cast<void*>(out + i)) Element(in[i]);
    } else {
        ::new (destination) T(*static_cast<const T*>(prototype));
    }
}

} // namespace MkwTls

template <typename T>
class MkwThreadLocal {
public:
    MkwThreadLocal() : prototype_{} { Init(); }

    template <typename U, typename = std::enable_if_t<std::is_constructible_v<T, U&&> && !std::is_array_v<T>>>
    MkwThreadLocal(U&& value) : prototype_(std::forward<U>(value)) { Init(); }

    MkwThreadLocal(const MkwThreadLocal&) = delete;
    MkwThreadLocal& operator=(const MkwThreadLocal&) = delete;

    T& get() const { return *static_cast<T*>(MkwTls::Slot(index_)); }
    operator T&() const { return get(); }

    template <typename U>
    MkwThreadLocal& operator=(U&& value)
    {
        get() = std::forward<U>(value);
        return *this;
    }

    // Pointer-like and array-like access.
    template <typename U = T>
    auto operator->() const -> std::enable_if_t<std::is_pointer_v<U>, U> { return get(); }
    template <typename U = T>
    auto operator->() const -> std::enable_if_t<!std::is_pointer_v<U> && !std::is_array_v<U>, U*> { return &get(); }
    template <typename I>
    decltype(auto) operator[](I index) const { return get()[index]; }

    // Compound assignment and increment/decrement (not reachable through the
    // conversion operator, because built-in assignment operators never apply
    // user-defined conversions to their left operand).
    template <typename U> MkwThreadLocal& operator+=(U&& v) { get() += std::forward<U>(v); return *this; }
    template <typename U> MkwThreadLocal& operator-=(U&& v) { get() -= std::forward<U>(v); return *this; }
    template <typename U> MkwThreadLocal& operator|=(U&& v) { get() |= std::forward<U>(v); return *this; }
    template <typename U> MkwThreadLocal& operator&=(U&& v) { get() &= std::forward<U>(v); return *this; }
    template <typename U> MkwThreadLocal& operator^=(U&& v) { get() ^= std::forward<U>(v); return *this; }
    template <typename U = T, typename = std::enable_if_t<!std::is_array_v<U>>>
    U& operator++() { return ++get(); }
    template <typename U = T, typename = std::enable_if_t<!std::is_array_v<U>>>
    U& operator--() { return --get(); }
    template <typename U = T, typename = std::enable_if_t<!std::is_array_v<U>>>
    U operator++(int) { return get()++; }
    template <typename U = T, typename = std::enable_if_t<!std::is_array_v<U>>>
    U operator--(int) { return get()--; }

private:
    void Init()
    {
        index_ = MkwTls::Register(sizeof(T), alignof(T), &MkwTls::CopyConstruct<T>, &prototype_);
    }

    T prototype_;
    uint32_t index_ = 0;
};

#endif
