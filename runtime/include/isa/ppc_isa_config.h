#pragma once

#include <atomic>
#include <cstdint>

#define MKW_RESTRICT __restrict
#if defined(__x86_64__)
#include <immintrin.h>
#elif defined(__aarch64__)
#include <arm_neon.h>
#else
// Any other host (e.g. 32-bit big-endian PowerPC on the Wii U) uses the portable
// scalar branches in ppc_isa_float.h / ppc_isa_quantized.h.
#define MKW_ISA_SCALAR_HOST 1
#endif

inline constexpr bool MkwStateFreeAbiEnabled(uint32_t) noexcept
{
    return true;
}

#if defined(_WIN32)
#define MKW_PPC_FORCE_INLINE __forceinline
#define MKW_PPC_NO_INLINE __declspec(noinline)
#define MKW_PPC_INTERNAL_CALL __regcall
#else
// __forceinline/__declspec are MS-extension keywords Clang only recognizes when targeting
// Windows (MSVC or mingw); native Linux Clang needs the GNU-attribute spellings instead.
// __regcall has no portable non-Windows equivalent worth chasing here - the extra register
// args it saves matter for the hot PPC interpreter loop on Windows, but plain calls are fine
// elsewhere.
#define MKW_PPC_FORCE_INLINE __attribute__((always_inline)) inline
#define MKW_PPC_NO_INLINE __attribute__((noinline))
#define MKW_PPC_INTERNAL_CALL
#endif
#define MKW_PPC_ALWAYS_INLINE_BODY __attribute__((always_inline))
#define MKW_PPC_COLD __attribute__((cold))


#if defined(__clang__)
using MkwStateFreeResult2 = uint64_t __attribute__((ext_vector_type(2)));
#else
// GCC has no ext_vector_type, and its vector_size types reject the emitted
// `return { a, b };`. A two-element aggregate accepts that brace return and the
// `result[0]` / `result[1]` reads the translator emits.
struct MkwStateFreeResult2 {
    uint64_t lanes[2];
    constexpr uint64_t operator[](int index) const noexcept { return lanes[index]; }
    uint64_t& operator[](int index) noexcept { return lanes[index]; }
};
#endif
