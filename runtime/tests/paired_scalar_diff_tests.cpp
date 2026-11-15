// Differential test for the paired-single helpers in isa/ppc_isa_float.h.
//
// Runs every ps_* helper over special values (zeros, denormals, infinities,
// quiet and signalling NaNs, FLT_MIN/FLT_MAX edges) plus pseudo-random inputs
// and prints one FNV-1a hash per operation. Build it once for a SIMD host
// (x86-64 SSE/FMA, or AArch64 NEON) and once for a scalar host (e.g. big-endian
// PowerPC under qemu-ppc): identical hashes mean the scalar branch is
// bit-identical to the reference implementation.
//
// FPSCR[NI] is left clear, so host flush-to-zero emulation is not exercised here.
// Operations with two or more NaN inputs to a fused multiply-add are skipped: the
// header documents that even two x86 builds may disagree on the winning NaN there.

#include "isa/ppc_isa_float.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

uint64_t Fnv(uint64_t hash, uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        hash ^= (value >> (i * 8)) & 0xFFu;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint64_t Bits(double value)
{
    return PpcBitCastToU64Inline(value);
}

bool IsNanBits(uint32_t bits)
{
    return (bits & 0x7FFFFFFFu) > 0x7F800000u;
}

int NanLanes(double value)
{
    const uint64_t bits = Bits(value);
    return (IsNanBits(static_cast<uint32_t>(bits)) ? 1 : 0) +
           (IsNanBits(static_cast<uint32_t>(bits >> 32)) ? 1 : 0);
}

uint32_t g_lcg = 0x12345678u;
uint32_t NextRandom()
{
    g_lcg = g_lcg * 1664525u + 1013904223u;
    return g_lcg;
}

std::vector<uint32_t> MakeLaneValues()
{
    std::vector<uint32_t> values = {
        0x00000000u, 0x80000000u,             // +-0
        0x3F800000u, 0xBF800000u,             // +-1
        0x00000001u, 0x80000001u,             // smallest denormals
        0x007FFFFFu, 0x807FFFFFu,             // largest denormals
        0x00800000u, 0x80800000u,             // +-FLT_MIN
        0x7F7FFFFFu, 0xFF7FFFFFu,             // +-FLT_MAX
        0x7F800000u, 0xFF800000u,             // +-inf
        0x7FC00000u, 0xFFC00000u,             // quiet NaNs
        0x7FC12345u, 0xFF812345u,             // quiet / signalling NaN with payload
        0x7F800001u, 0xFF800001u,             // signalling NaNs
        0x40490FDBu, 0xC2F6E979u, 0x3EAAAAABu, 0x4B7FFFFFu, 0x33800000u,
    };
    for (int i = 0; i < 40; ++i) {
        // Mostly finite values with moderate exponents, so arithmetic is exercised.
        const uint32_t r = NextRandom();
        const uint32_t exponent = 100u + (r >> 24) % 56u;
        values.push_back((r & 0x80000000u) | (exponent << 23) | (NextRandom() & 0x007FFFFFu));
    }
    return values;
}

double Pack(uint32_t ps0, uint32_t ps1)
{
    return PpcBitCastToDoubleInline((static_cast<uint64_t>(ps0) << 32) | ps1);
}

struct OpHash {
    const char* name;
    uint64_t hash = 1469598103934665603ull;
    unsigned count = 0;
    void Add(double value)
    {
        hash = Fnv(hash, Bits(value));
        ++count;
    }
    void Add(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        hash = Fnv(hash, bits);
        ++count;
    }
};

}  // namespace

int main()
{
    const std::vector<uint32_t> lanes = MakeLaneValues();
    std::vector<double> pairs;
    for (size_t i = 0; i < lanes.size(); ++i)
        pairs.push_back(Pack(lanes[i], lanes[(i * 7 + 3) % lanes.size()]));

    OpHash mul{"ps_mul"}, add{"ps_add"}, sub{"ps_sub"}, div{"ps_div"};
    OpHash muls0{"ps_muls0"}, muls1{"ps_muls1"};
    OpHash madd{"ps_madd"}, msub{"ps_msub"}, nmadd{"ps_nmadd"}, nmsub{"ps_nmsub"};
    OpHash madds0{"ps_madds0"}, madds1{"ps_madds1"};
    OpHash merge{"ps_merge*"}, sel{"ps_sel"}, lanesOp{"get/pack/fromscalar"};

    for (const double a : pairs) {
        lanesOp.Add(PpcGetPs0Inline(a));
        lanesOp.Add(PpcGetPs1Inline(a));
        lanesOp.Add(PpcPackPairedInline(PpcGetPs1Inline(a), PpcGetPs0Inline(a)));
        lanesOp.Add(PPC_PsFromScalarNoNiInline(PpcBitCastToDoubleInline(0x3FF0000000000001ull ^ (Bits(a) & 0x8000000000000000ull))));
        for (const double b : pairs) {
            mul.Add(PPC_PsMulNoNiInline(a, b));
            add.Add(PPC_PsAddNoNiInline(a, b));
            sub.Add(PPC_PsSubNoNiInline(a, b));
            div.Add(PPC_PsDivInline(a, b));
            muls0.Add(PPC_PsMuls0Inline(a, b));
            muls1.Add(PPC_PsMuls1Inline(a, b));
            merge.Add(PPC_PsMerge00Inline(a, b));
            merge.Add(PPC_PsMerge01Inline(a, b));
            merge.Add(PPC_PsMerge10Inline(a, b));
            merge.Add(PPC_PsMerge11Inline(a, b));
        }
    }

    // Fused ops: a subset of triples keeps the run short.
    for (size_t i = 0; i < pairs.size(); ++i) {
        for (size_t j = 0; j < pairs.size(); j += 2) {
            for (size_t k = 0; k < pairs.size(); k += 3) {
                const double a = pairs[i], c = pairs[j], b = pairs[k];
                sel.Add(PPC_PsSelInline(a, c, b));
                if (NanLanes(a) + NanLanes(c) + NanLanes(b) > 1)
                    continue;  // multi-NaN winner is not deterministic on x86 either
                madd.Add(PPC_PsMaddNoNiInline(a, c, b));
                msub.Add(PPC_PsMsubNoNiInline(a, c, b));
                nmadd.Add(PPC_PsNmaddInline(a, c, b));
                nmsub.Add(PPC_PsNmsubNoNiInline(a, c, b));
                madds0.Add(PPC_PsMadds0Inline(a, c, b));
                madds1.Add(PPC_PsMadds1Inline(a, c, b));
            }
        }
    }

    const OpHash* all[] = {&mul, &add, &sub, &div, &muls0, &muls1, &madd, &msub, &nmadd, &nmsub,
                           &madds0, &madds1, &merge, &sel, &lanesOp};
    for (const OpHash* op : all)
        std::printf("%-22s %8u %016llx\n", op->name, op->count, static_cast<unsigned long long>(op->hash));
    return 0;
}
