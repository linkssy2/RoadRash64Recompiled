// RR64 SIMD correctness harness and microbenchmark.
//
// Every kernel in rt64_rr64_simd.cpp is run at every level this CPU supports
// (scalar, vec128, AVX, AVX2 on x86; scalar, NEON on arm64) and compared
// byte-for-byte against the scalar reference, which is the original loop.
// No ROM or game data is used. Exit status is nonzero on any mismatch.
//
//   RR64SimdHarness            correctness only
//   RR64SimdHarness --bench    correctness, then timings per level
//
// The arm64 build can be exercised on an x86 host under qemu-user; timings
// from an emulator are meaningless, correctness is not.

#include "common/rt64_rr64_simd.h"

#include <algorithm>
#include <chrono>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <limits>
#include <random>
#include <string>
#include <vector>

#if defined(RR64_SIMD_X64)
#include <emmintrin.h>
#endif

using namespace RT64;
using RR64Simd::Level;

namespace {
    std::mt19937_64 rng(0x5252363453494D44ull); // "RR64SIMD"
    long failures = 0;
    long comparisons = 0; // element-level comparisons

    uint32_t u32() { return uint32_t(rng()); }

    float bitsToFloat(uint32_t b) {
        float f;
        std::memcpy(&f, &b, sizeof(f));
        return f;
    }

    uint32_t floatToBits(float f) {
        uint32_t b;
        std::memcpy(&b, &f, sizeof(b));
        return b;
    }

    std::vector<Level> levels() {
        std::vector<Level> out = { Level::Scalar };
#if defined(RR64_SIMD_X64)
        out.push_back(Level::Vec128);
        if (RR64Simd::cpuHasAVX()) out.push_back(Level::AVX);
        if (RR64Simd::cpuHasAVX2()) out.push_back(Level::AVX2);
#elif defined(RR64_SIMD_ARM64)
        out.push_back(Level::Vec128);
#endif
        return out;
    }

    void fail(const char *kernel, Level level, const std::string &detail) {
        if (failures < 40) {
            std::printf("  MISMATCH %-26s %-16s %s\n", kernel, RR64Simd::levelName(level), detail.c_str());
        }
        failures++;
    }

    // Bit-identical, or both NaN.
    bool sameValue(float a, float b) {
        return (floatToBits(a) == floatToBits(b)) || (std::isnan(a) && std::isnan(b));
    }

    template <typename T>
    bool sameBytes(const std::vector<T> &a, const std::vector<T> &b) {
        comparisons += long(a.size());
        return (a.size() == b.size()) && (std::memcmp(a.data(), b.data(), a.size() * sizeof(T)) == 0);
    }

    // Random float drawn from a mix of magnitudes plus specials.
    float interestingFloat(bool allowSpecials) {
        switch (rng() % 12) {
        case 0: return 0.0f;
        case 1: return -0.0f;
        case 2: return bitsToFloat(u32() & 0x807FFFFFu); // denormal (or zero)
        case 3: return allowSpecials ? std::numeric_limits<float>::infinity() : 1.0f;
        case 4: return allowSpecials ? -std::numeric_limits<float>::infinity() : -1.0f;
        case 5: return allowSpecials ? bitsToFloat(0x7FC00000u | (u32() & 0x3FFFFF)) : 2.0f;
        case 6: return float(int16_t(u32()));
        case 7: return std::ldexp(float(int32_t(u32() % 2000001) - 1000000) / 1000000.0f, int(u32() % 60) - 30);
        default: {
            // Any finite pattern.
            uint32_t b = u32();
            if ((b & 0x7F800000u) == 0x7F800000u) b &= 0xBF7FFFFFu;
            return bitsToFloat(b);
        }
        }
    }

    // ---------------------------------------------------------------------

    void testVertexAttributes() {
        for (int trial = 0; trial < 4000; trial++) {
            const uint32_t count = trial % 71;
            std::vector<uint8_t> records(size_t(count) * 16 + 16);
            for (auto &b : records) b = uint8_t(u32());
            const bool fog = (trial & 1) != 0;
            std::vector<float> refPos(size_t(count) * 3 + 4, -7.0f);
            std::vector<uint8_t> refCol(size_t(count) * 4 + 4, 0xCD);
            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            RR64Simd::convertVertexAttributes(records.data(), count, fog, refPos.data(), refCol.data());
            for (Level l : levels()) {
                std::vector<float> pos(refPos.size(), -7.0f);
                std::vector<uint8_t> col(refCol.size(), 0xCD);
                RR64Simd::forceLevelForTesting(int(l));
                RR64Simd::convertVertexAttributes(records.data(), count, fog, pos.data(), col.data());
                // Sentinels past the end must survive: no kernel may overrun.
                if (!sameBytes(pos, refPos)) fail("convertVertexAttributes.pos", l, "count=" + std::to_string(count));
                if (!sameBytes(col, refCol)) fail("convertVertexAttributes.col", l, "count=" + std::to_string(count));
            }
        }
    }

    void testTransform() {
        for (int trial = 0; trial < 6000; trial++) {
            const uint32_t count = trial % 67;
            float mvp[16];
            const int mode = trial % 9;
            for (int k = 0; k < 16; k++) {
                if (mode < 5) {
                    mvp[k] = std::uniform_real_distribution<float>(-2.0f, 2.0f)(rng);
                }
                else if (mode == 5) {
                    mvp[k] = std::uniform_real_distribution<float>(-1e-3f, 1e-3f)(rng);
                }
                else {
                    mvp[k] = interestingFloat(mode == 8);
                }
            }

            if (trial % 11 == 0) {
                // Degenerate projection: w column all zero -> x/0, 0/0.
                mvp[3] = mvp[7] = mvp[11] = mvp[15] = 0.0f;
            }

            float scale[4] = { std::uniform_real_distribution<float>(-400.0f, 400.0f)(rng), std::uniform_real_distribution<float>(-400.0f, 400.0f)(rng), std::uniform_real_distribution<float>(0.0f, 1.0f)(rng), 0.0f };
            float translate[4] = { std::uniform_real_distribution<float>(0.0f, 640.0f)(rng), std::uniform_real_distribution<float>(0.0f, 480.0f)(rng), std::uniform_real_distribution<float>(0.0f, 1.0f)(rng), 0.0f };
            if (trial % 5 == 0) {
                // The padding lane is 0 in RT64's viewport, but exercise it anyway.
                scale[3] = interestingFloat(false);
                translate[3] = interestingFloat(false);
            }

            std::vector<float> pos(size_t(count) * 3 + 1);
            for (float &p : pos) {
                // Positions are int16 conversions or extended-segment floats.
                p = (trial % 3 == 0) ? interestingFloat(false) : float(int16_t(u32()));
            }

            std::vector<float> refT(size_t(count) * 4 + 4, -3.0f), refS(size_t(count) * 4 + 4, -3.0f);
            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            RR64Simd::transformVertices(pos.data(), count, mvp, scale, translate, refT.data(), refS.data());
            for (Level l : levels()) {
                std::vector<float> t(refT.size(), -3.0f), s(refS.size(), -3.0f);
                RR64Simd::forceLevelForTesting(int(l));
                RR64Simd::transformVertices(pos.data(), count, mvp, scale, translate, t.data(), s.data());
                // Both records, all four floats (the screen padding lane too),
                // bit-exact. NaN outputs (only reachable from degenerate
                // matrices) must be NaN in both, but their sign/payload is not
                // compared: LLVM and MSVC may commute operands or fold x / -w into
                // -(x / w), so even the scalar reference does not pin NaN bits.
                comparisons += long(count) * 8;
                bool clipOk = true, screenOk = true;
                for (uint32_t v = 0; v < count; v++) {
                    for (int k = 0; k < 4; k++) {
                        if (!sameValue(t[v * 4 + k], refT[v * 4 + k])) clipOk = false;
                        if (!sameValue(s[v * 4 + k], refS[v * 4 + k])) screenOk = false;
                    }
                }

                for (size_t k = size_t(count) * 4; k < t.size(); k++) {
                    // Sentinels past the end: no overrun.
                    if (floatToBits(t[k]) != floatToBits(refT[k])) clipOk = false;
                    if (floatToBits(s[k]) != floatToBits(refS[k])) screenOk = false;
                }

                if (!clipOk) fail("transformVertices.clip", l, "count=" + std::to_string(count) + " mode=" + std::to_string(mode));
                if (!screenOk) fail("transformVertices.screen", l, "count=" + std::to_string(count) + " mode=" + std::to_string(mode));
            }
        }
    }

    void testTexcoords() {
        // Exhaustive over every int16 s/t for a spread of scale factors.
        std::vector<int32_t> scales = { 0, 1, 2, 3, 0x7FFF, 0x8000, 0x8001, 0xFFFE, 0xFFFF, 0x1000, 0x0800, 0x0400 };
        while (scales.size() < 48) scales.push_back(int32_t(u32() & 0xFFFF));
        const uint32_t count = 65536 / 2;
        std::vector<uint8_t> records(size_t(count) * 16);
        for (size_t si = 0; si < scales.size(); si += 2) {
            for (int half = 0; half < 2; half++) {
                for (uint32_t j = 0; j < count; j++) {
                    const int16_t s = int16_t(j * 2 + half);
                    const int16_t t = int16_t(~(j * 2 + half));
                    uint8_t *v = &records[size_t(j) * 16];
                    for (int k = 0; k < 16; k++) v[k] = uint8_t(u32());
                    std::memcpy(v + 10, &s, 2);
                    std::memcpy(v + 8, &t, 2);
                }

                const int32_t sc = scales[si], tc = scales[si + 1];
                std::vector<float> ref(size_t(count) * 2 + 2, 9.0f);
                RR64Simd::forceLevelForTesting(int(Level::Scalar));
                RR64Simd::computeTexcoords(records.data(), count, sc, tc, ref.data());
                for (Level l : levels()) {
                    std::vector<float> out(ref.size(), 9.0f);
                    RR64Simd::forceLevelForTesting(int(l));
                    RR64Simd::computeTexcoords(records.data(), count, sc, tc, out.data());
                    if (!sameBytes(out, ref)) fail("computeTexcoords", l, "sc=" + std::to_string(sc) + " tc=" + std::to_string(tc));
                }
            }
        }

        // Odd packet sizes for the tails.
        for (uint32_t count2 = 0; count2 < 40; count2++) {
            std::vector<uint8_t> r(size_t(count2) * 16 + 1);
            for (auto &b : r) b = uint8_t(u32());
            std::vector<float> ref(size_t(count2) * 2 + 2, 9.0f);
            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            RR64Simd::computeTexcoords(r.data(), count2, 0xFFFF, 0x8000, ref.data());
            for (Level l : levels()) {
                std::vector<float> out(ref.size(), 9.0f);
                RR64Simd::forceLevelForTesting(int(l));
                RR64Simd::computeTexcoords(r.data(), count2, 0xFFFF, 0x8000, out.data());
                if (!sameBytes(out, ref)) fail("computeTexcoords.tail", l, "count=" + std::to_string(count2));
            }
        }
    }

    void testGeometryScans() {
        for (int trial = 0; trial < 20000; trial++) {
            const size_t count = size_t(trial % 97);
            std::vector<float> values(count);
            const bool specials = (trial % 4) == 0;
            for (float &v : values) v = interestingFloat(false);
            if (specials && count) values[u32() % count] = interestingFloat(true);

            // canonicalizePositionBits
            std::vector<uint32_t> ref(count + 1, 0xA5A5A5A5u);
            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            const bool refOk = RR64Simd::canonicalizePositionBits(values.data(), ref.data(), count);
            const bool refFinite = RR64Simd::allFinite(values.data(), count);
            for (Level l : levels()) {
                std::vector<uint32_t> out(count + 1, 0xA5A5A5A5u);
                RR64Simd::forceLevelForTesting(int(l));
                const bool ok = RR64Simd::canonicalizePositionBits(values.data(), out.data(), count);
                comparisons++;
                if (ok != refOk) fail("canonicalizePositionBits.ret", l, "count=" + std::to_string(count));
                // Output is defined only on success (the caller discards it otherwise).
                if (refOk && !sameBytes(out, ref)) fail("canonicalizePositionBits", l, "count=" + std::to_string(count));
                comparisons++;
                if (RR64Simd::allFinite(values.data(), count) != refFinite) fail("allFinite", l, "count=" + std::to_string(count));
            }

            // floatsEqual: identical copy, signed-zero flips, one perturbed lane.
            std::vector<float> other = values;
            for (float &v : other) {
                if ((v == 0.0f) && (u32() & 1)) v = -v;
            }

            if ((trial % 3 == 0) && count) {
                const size_t k = u32() % count;
                other[k] = std::nextafter(other[k], std::numeric_limits<float>::infinity());
            }

            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            const bool refEq = RR64Simd::floatsEqual(values.data(), other.data(), count);
            for (Level l : levels()) {
                RR64Simd::forceLevelForTesting(int(l));
                comparisons++;
                if (RR64Simd::floatsEqual(values.data(), other.data(), count) != refEq) fail("floatsEqual", l, "count=" + std::to_string(count));
            }

            // indicesInRange / offsetIndicesEqual
            const uint32_t firstChoices[] = { 0u, 1u, 7u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFF0u, 0xFFFFFFFFu, u32() };
            const uint32_t first = firstChoices[trial % 8];
            const uint64_t vcChoices[] = { 0ull, 1ull, 3ull, 64ull, 0x80000000ull, 0xFFFFFFFFull, 0x100000000ull, uint64_t(u32() % 5000) };
            const uint64_t vertexCount = vcChoices[(trial / 8) % 8];
            std::vector<uint32_t> indices(count);
            for (uint32_t &i : indices) {
                const uint32_t span = vertexCount ? uint32_t(std::min<uint64_t>(vertexCount, 0xFFFFFFFFull)) : 1u;
                i = first + (u32() % span);
            }

            if ((trial % 5 == 0) && count) {
                const uint32_t edge[] = { first - 1, uint32_t(first + vertexCount), uint32_t(first + vertexCount - 1), 0u, 0xFFFFFFFFu };
                indices[u32() % count] = edge[u32() % 5];
            }

            RR64Simd::forceLevelForTesting(int(Level::Scalar));
            const bool refIn = RR64Simd::indicesInRange(indices.data(), count, first, vertexCount);
            std::vector<uint32_t> shifted(count);
            const uint32_t firstB = u32();
            for (size_t k = 0; k < count; k++) shifted[k] = indices[k] - first + firstB;
            if ((trial % 7 == 0) && count) shifted[u32() % count] ^= 1u << (u32() % 32);
            const bool refOff = RR64Simd::offsetIndicesEqual(indices.data(), shifted.data(), count, first, firstB);
            for (Level l : levels()) {
                RR64Simd::forceLevelForTesting(int(l));
                comparisons += 2;
                if (RR64Simd::indicesInRange(indices.data(), count, first, vertexCount) != refIn) fail("indicesInRange", l, "count=" + std::to_string(count) + " first=" + std::to_string(first) + " vc=" + std::to_string(vertexCount));
                if (RR64Simd::offsetIndicesEqual(indices.data(), shifted.data(), count, first, firstB) != refOff) fail("offsetIndicesEqual", l, "count=" + std::to_string(count));
            }
        }
    }

    void testPalettePresence() {
        for (int trial = 0; trial < 3000; trial++) {
            const size_t count = size_t(u32() % 4096);
            std::vector<uint8_t> data(count);
            const uint32_t span = 1u + (u32() % 256);
            const uint32_t base = u32() % 256;
            for (auto &b : data) b = uint8_t(base + (u32() % span));
            // Original TMEMHasher v5 CI8 loop.
            uint64_t expect[4] = { u32(), 0, 0, uint64_t(u32()) << 32 };
            uint64_t start[4];
            std::memcpy(start, expect, sizeof(start));
            for (size_t i = 0; i < count; i++) expect[data[i] >> 6U] |= (1ULL << (data[i] & 0x3FU));
            for (Level l : levels()) {
                RR64Simd::forceLevelForTesting(int(l));
                uint8_t seen[256] = {};
                uint64_t got[4];
                std::memcpy(got, start, sizeof(got));
                RR64Simd::markBytesPresent(data.data(), count, seen);
                RR64Simd::packBytesPresent(seen, got);
                comparisons += 256;
                if (std::memcmp(got, expect, sizeof(got)) != 0) fail("palettePresence", l, "count=" + std::to_string(count));
            }
        }
    }

    void testTMEMSwizzle() {
#if defined(RR64_SIMD_HAS_TMEM_SWIZZLE)
        for (int trial = 0; trial < 200000; trial++) {
            uint8_t src[16];
            for (auto &b : src) b = uint8_t(u32());

            // Palette pair: scalar loadWord<TLUT> semantics for an aligned address.
            uint32_t source;
            std::memcpy(&source, src, 4);
            uint8_t expectP[16];
            for (int w = 0; w < 2; w++) {
                const uint8_t lo = src[(w * 2) ^ 3], hi = src[(w * 2 + 1) ^ 3];
                for (int k = 0; k < 4; k++) {
                    expectP[w * 8 + k * 2 + 0] = lo;
                    expectP[w * 8 + k * 2 + 1] = hi;
                }
            }

            uint8_t gotP[16];
            RR64Simd::tmemPaletteWordPair(source, gotP);
            comparisons += 16;
            if (std::memcmp(gotP, expectP, 16) != 0) fail("tmemPaletteWordPair", Level::Vec128, "");

            // Texel pair: scalar loadWord (non-TLUT) byte mapping, both row parities.
            for (uint32_t xorMask : { 0u, 4u }) {
                uint8_t expectT[16];
                for (int word = 0; word < 2; word++) {
                    for (uint32_t i = 0; i < 8; i++) {
                        expectT[word * 8 + (i ^ xorMask)] = src[(word * 8 + i) ^ 3];
                    }
                }

                uint8_t gotT[16];
                RR64Simd::tmemTexelWordPair(src, gotT, xorMask != 0);
                comparisons += 16;
                if (std::memcmp(gotT, expectT, 16) != 0) fail("tmemTexelWordPair", Level::Vec128, "xor=" + std::to_string(xorMask));

#   if defined(RR64_SIMD_X64)
                // Also prove equivalence with the SSE2 sequence it replaces.
                __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
                bytes = _mm_or_si128(_mm_slli_epi16(bytes, 8), _mm_srli_epi16(bytes, 8));
                bytes = _mm_shufflelo_epi16(bytes, _MM_SHUFFLE(2, 3, 0, 1));
                bytes = _mm_shufflehi_epi16(bytes, _MM_SHUFFLE(2, 3, 0, 1));
                if (xorMask) bytes = _mm_shuffle_epi32(bytes, _MM_SHUFFLE(2, 3, 0, 1));
                uint8_t sse2[16];
                _mm_storeu_si128(reinterpret_cast<__m128i *>(sse2), bytes);
                comparisons += 16;
                if (std::memcmp(gotT, sse2, 16) != 0) fail("tmemTexelWordPair.vsSSE2", Level::Vec128, "");
#   endif
            }

#   if defined(RR64_SIMD_X64)
            __m128i p = _mm_cvtsi32_si128(static_cast<int32_t>(source));
            p = _mm_or_si128(_mm_slli_epi16(p, 8), _mm_srli_epi16(p, 8));
            p = _mm_shufflelo_epi16(p, _MM_SHUFFLE(2, 3, 0, 1));
            p = _mm_unpacklo_epi16(p, p);
            p = _mm_unpacklo_epi32(p, p);
            uint8_t sse2P[16];
            _mm_storeu_si128(reinterpret_cast<__m128i *>(sse2P), p);
            comparisons += 16;
            if (std::memcmp(gotP, sse2P, 16) != 0) fail("tmemPaletteWordPair.vsSSE2", Level::Vec128, "");
#   endif
        }
#endif
    }

    // ---------------------------------------------------------------------
    // TMEM texel runs (tuning pass 2)
    //
    // Model of RDP::loadToTMEMCommon<RGBA32 = false, BLOCK, TLUT = false>.
    // `runs` selects the new run path (the code block in rt64_rdp.cpp,
    // reproduced here); without it every word goes through the original
    // one-word byte loop. Both start from the same TMEM contents and must
    // leave TMEM byte-identical for random LoadTile/LoadBlock parameters,
    // including TMEM wraps, DXT bank swaps, odd word counts and unaligned
    // sources.
#if defined(RR64_SIMD_HAS_TMEM_SWIZZLE)
    constexpr uint32_t ModelTmemBytes = 4096, ModelTmemMask = 4095;

    template <bool BLOCK>
    void modelLoadToTMEM(uint8_t *TMEM, const uint8_t *RDRAM, uint32_t textureStart, uint32_t textureStride, uint32_t tmemStart,
        uint32_t tmemStride, uint32_t wordsPerRow, uint32_t rowCount, uint32_t dxtIncrement, bool runs, bool pairs = false) {
        const uint32_t DXTSwap = 0x800;
        const uint32_t tmemMask = ModelTmemMask, tmemAdvance = 0x8, textureAdvance = 0x8;
        uint32_t textureAddress, tmemAddress, wordCount;
        uint32_t tmemXorMask = 0x0;
        uint32_t dxtCounter = 0x0;
        auto loadWordStep = [&]() {
            if constexpr (BLOCK) {
                dxtCounter += dxtIncrement;
                while (dxtCounter >= DXTSwap) {
                    tmemAddress = (tmemAddress + tmemStride) & tmemMask;
                    dxtCounter -= DXTSwap;
                    tmemXorMask ^= 0x4;
                }
            }

            textureAddress += textureAdvance;
            tmemAddress = (tmemAddress + tmemAdvance) & tmemMask;
        };

        uint32_t textureAddressRow = textureStart;
        uint32_t tmemAddressRow = tmemStart & tmemMask;
        while (rowCount > 0) {
            textureAddress = textureAddressRow;
            tmemAddress = tmemAddressRow;
            wordCount = wordsPerRow;
            while (wordCount > 0) {
                if (runs && wordCount >= 2 && (textureAddress & 3u) == 0u) {
                    uint32_t run = std::min(wordCount, (ModelTmemBytes - tmemAddress) / tmemAdvance);
                    if constexpr (BLOCK) {
                        if (dxtIncrement > 0) {
                            const uint32_t stepsWithoutSwap = (DXTSwap - 1u - dxtCounter) / dxtIncrement;
                            run = std::min(run, stepsWithoutSwap + 1u);
                        }
                    }

                    if (run >= 2) {
                        RR64Simd::tmemTexelWords(RDRAM + textureAddress, TMEM + tmemAddress, run, tmemXorMask != 0);
                        textureAddress += textureAdvance * (run - 1u);
                        tmemAddress = (tmemAddress + tmemAdvance * (run - 1u)) & tmemMask;
                        if constexpr (BLOCK) {
                            dxtCounter += dxtIncrement * (run - 1u);
                        }

                        loadWordStep();
                        wordCount -= run;
                        continue;
                    }
                }

                // The pass-1 two-word path, for the benchmark baseline only.
                const bool contiguous = !BLOCK || (dxtCounter + dxtIncrement < DXTSwap);
                if (pairs && wordCount >= 2 && (textureAddress & 3u) == 0u && tmemAddress <= ModelTmemBytes - 16u && contiguous) {
                    RR64Simd::tmemTexelWordPair(RDRAM + textureAddress, TMEM + tmemAddress, tmemXorMask != 0);
                    loadWordStep();
                    loadWordStep();
                    wordCount -= 2;
                    continue;
                }

                // Original one-word byte loop (loadWord, non-TLUT, non-RGBA32).
                for (uint32_t i = 0; i < 8; i++) {
                    TMEM[(tmemAddress + i) ^ tmemXorMask] = RDRAM[(textureAddress + i) ^ 3];
                }

                loadWordStep();
                wordCount--;
            }

            tmemAddressRow = (tmemAddressRow + tmemStride) & tmemMask;
            textureAddressRow += textureStride;
            tmemXorMask ^= 0x4;
            rowCount--;
        }
    }
#endif

    void testTMEMRuns() {
#if defined(RR64_SIMD_HAS_TMEM_SWIZZLE)
        // The run kernel itself against the per-word mapping, every length.
        std::vector<uint8_t> src(64 * 8 + 8), want(64 * 8 + 8), got(64 * 8 + 8);
        for (uint32_t words = 1; words <= 64; words++) {
            for (uint32_t xorMask : { 0u, 4u }) {
                for (auto &b : src) b = uint8_t(u32());
                std::fill(want.begin(), want.end(), 0xEE);
                for (uint32_t w = 0; w < words; w++) {
                    for (uint32_t i = 0; i < 8; i++) want[w * 8 + (i ^ xorMask)] = src[(w * 8 + i) ^ 3];
                }

                for (Level l : levels()) {
                    if (l == Level::Scalar) continue;
                    RR64Simd::forceLevelForTesting(int(l));
                    std::fill(got.begin(), got.end(), 0xEE);
                    RR64Simd::tmemTexelWords(src.data(), got.data(), words, xorMask != 0);
                    if (!sameBytes(got, want)) fail("tmemTexelWords", l, "words=" + std::to_string(words) + " xor=" + std::to_string(xorMask));
                }
            }
        }

        // Whole-load equivalence, LoadTile and LoadBlock shapes.
        std::vector<uint8_t> rdram(1 << 16);
        for (auto &b : rdram) b = uint8_t(u32());
        std::vector<uint8_t> tmemRef(ModelTmemBytes), tmemNew(ModelTmemBytes);
        for (int trial = 0; trial < 20000; trial++) {
            const bool block = (trial & 1) != 0;
            const uint32_t tmemStart = (u32() % 512) * 8;
            const uint32_t tmemStride = block ? (u32() % 8) * 8 : ((u32() % 4 == 0) ? 0 : (u32() % 64) * 8);
            const uint32_t wordsPerRow = block ? 1 + u32() % 600 : 1 + u32() % 40;
            const uint32_t rowCount = block ? 1 : 1 + u32() % 24;
            const uint32_t textureStride = (u32() % 48) * 8 + (u32() % 3 == 0 ? (u32() % 4) : 0);
            uint32_t textureStart = u32() % 8192;
            if (trial % 3 != 0) textureStart &= ~3u; // mostly aligned, some not
            const uint32_t dxtChoices[] = { 0u, 1u, 0x80u, 0x100u, 0x200u, 0x2AAu, 0x400u, 0x7FFu, 0x800u, 0x900u, u32() % 0x1000 };
            const uint32_t dxtIncrement = block ? dxtChoices[u32() % 11] : 0;
            for (auto &b : tmemRef) b = uint8_t(u32());
            for (Level l : levels()) {
                if (l == Level::Scalar) continue;
                RR64Simd::forceLevelForTesting(int(l));
                std::vector<uint8_t> start = tmemRef;
                tmemNew = start;
                std::vector<uint8_t> expect = start;
                if (block) {
                    modelLoadToTMEM<true>(expect.data(), rdram.data(), textureStart, textureStride, tmemStart, tmemStride, wordsPerRow, rowCount, dxtIncrement, false);
                    modelLoadToTMEM<true>(tmemNew.data(), rdram.data(), textureStart, textureStride, tmemStart, tmemStride, wordsPerRow, rowCount, dxtIncrement, true);
                }
                else {
                    modelLoadToTMEM<false>(expect.data(), rdram.data(), textureStart, textureStride, tmemStart, tmemStride, wordsPerRow, rowCount, 0, false);
                    modelLoadToTMEM<false>(tmemNew.data(), rdram.data(), textureStart, textureStride, tmemStart, tmemStride, wordsPerRow, rowCount, 0, true);
                }

                if (!sameBytes(tmemNew, expect)) {
                    fail("loadToTMEM runs", l, std::string(block ? "block" : "tile") + " words=" + std::to_string(wordsPerRow) +
                        " rows=" + std::to_string(rowCount) + " tmem=" + std::to_string(tmemStart) + " dxt=" + std::to_string(dxtIncrement));
                }
            }
        }
#endif
    }

    // ---------------------------------------------------------------------

    template <typename F>
    double bestNs(F &&fn, int reps, double unitCount) {
        double best = 1e30;
        for (int r = 0; r < 7; r++) {
            const auto t0 = std::chrono::steady_clock::now();
            for (int i = 0; i < reps; i++) fn();
            const auto t1 = std::chrono::steady_clock::now();
            best = std::min(best, std::chrono::duration<double, std::nano>(t1 - t0).count() / (reps * unitCount));
        }

        return best;
    }

    volatile uint64_t sink = 0;

    void bench() {
        std::printf("\nTimings (best of 7, ns per element; speedup vs scalar)\n");
        // A typical F3DEX2 packet is 32 vertices; terrain loads full buffers.
        const uint32_t packet = 32;
        std::vector<uint8_t> records(size_t(packet) * 16);
        for (auto &b : records) b = uint8_t(u32());
        std::vector<float> pos(packet * 3), tf(packet * 4), sc(packet * 4), tc(packet * 2);
        std::vector<uint8_t> col(packet * 4);
        for (float &p : pos) p = float(int16_t(u32()));
        float mvp[16];
        for (float &m : mvp) m = std::uniform_real_distribution<float>(-2.0f, 2.0f)(rng);
        float vs[4] = { 320, -240, 0.5f, 0 }, vt[4] = { 320, 240, 0.5f, 0 };

        const size_t geo = 30000; // 10k vertices of positions
        std::vector<float> g(geo), g2;
        for (float &v : g) v = float(int16_t(u32())) * 0.25f;
        g2 = g;
        std::vector<uint32_t> gbits(geo), idx(geo), idx2(geo);
        for (size_t i = 0; i < geo; i++) {
            idx[i] = 1000 + uint32_t(i % 9000);
            idx2[i] = idx[i] - 1000 + 77;
        }

        std::vector<uint8_t> ci8(2048);
        for (auto &b : ci8) b = uint8_t(u32() % 40);
        std::vector<uint8_t> tmemSrc(8192), tmemBench(4096);
        for (auto &b : tmemSrc) b = uint8_t(u32());

        struct Row {
            const char *name;
            double units;
            int reps;
            std::function<void()> fn;
        };

        std::vector<Row> rows = {
            { "vertex attributes (32/packet)", double(packet), 20000, [&] { RR64Simd::convertVertexAttributes(records.data(), packet, false, pos.data(), col.data()); } },
            { "vertex transform (32/packet)", double(packet), 20000, [&] { RR64Simd::transformVertices(pos.data(), packet, mvp, vs, vt, tf.data(), sc.data()); } },
            { "texcoords (32/packet)", double(packet), 20000, [&] { RR64Simd::computeTexcoords(records.data(), packet, 0x8000, 0x8000, tc.data()); } },
            { "canonicalize position bits", double(geo), 200, [&] { sink += RR64Simd::canonicalizePositionBits(g.data(), gbits.data(), geo); } },
            { "allFinite", double(geo), 400, [&] { sink += RR64Simd::allFinite(g.data(), geo); } },
            { "floatsEqual", double(geo), 400, [&] { sink += RR64Simd::floatsEqual(g.data(), g2.data(), geo); } },
            { "indicesInRange", double(geo), 400, [&] { sink += RR64Simd::indicesInRange(idx.data(), geo, 1000, 9000); } },
            { "offsetIndicesEqual", double(geo), 400, [&] { sink += RR64Simd::offsetIndicesEqual(idx.data(), idx2.data(), geo, 1000, 77); } },
            { "CI8 palette presence (2 KB)", 2048.0, 4000, [&] {
                uint8_t seen[256] = {};
                uint64_t bits[4] = {};
                RR64Simd::markBytesPresent(ci8.data(), ci8.size(), seen);
                RR64Simd::packBytesPresent(seen, bits);
                sink += bits[0];
            } },
#if defined(RR64_SIMD_HAS_TMEM_SWIZZLE)
            // 4 KB LoadTile (32 rows of 16 words) and 4 KB LoadBlock: per-word
            // path vs runs. Reported per TMEM byte.
            { "TMEM LoadTile 4 KB, word pairs", 4096.0, 2000, [&] {
                modelLoadToTMEM<false>(tmemBench.data(), tmemSrc.data(), 0, 128, 0, 128, 16, 32, 0, false, true); sink += tmemBench[7]; } },
            { "TMEM LoadTile 4 KB, runs", 4096.0, 2000, [&] {
                modelLoadToTMEM<false>(tmemBench.data(), tmemSrc.data(), 0, 128, 0, 128, 16, 32, 0, true); sink += tmemBench[7]; } },
            { "TMEM LoadBlock 4 KB, word pairs", 4096.0, 2000, [&] {
                modelLoadToTMEM<true>(tmemBench.data(), tmemSrc.data(), 0, 0, 0, 128, 512, 1, 0x80, false, true); sink += tmemBench[7]; } },
            { "TMEM LoadBlock 4 KB, runs", 4096.0, 2000, [&] {
                modelLoadToTMEM<true>(tmemBench.data(), tmemSrc.data(), 0, 0, 0, 128, 512, 1, 0x80, true); sink += tmemBench[7]; } },
#endif
            { "CI8 palette presence, original", 2048.0, 4000, [&] {
                uint64_t bits[4] = {};
                for (size_t i = 0; i < ci8.size(); i++) bits[ci8[i] >> 6U] |= (1ULL << (ci8[i] & 0x3FU));
                sink += bits[0];
            } },
        };

        const auto ls = levels();
        std::printf("  %-34s", "kernel");
        for (Level l : ls) std::printf(" %18s", RR64Simd::levelName(l));
        std::printf("\n");
        for (auto &row : rows) {
            std::printf("  %-34s", row.name);
            double scalarNs = 0.0;
            for (Level l : ls) {
                RR64Simd::forceLevelForTesting(int(l));
                const double ns = bestNs(row.fn, row.reps, row.units);
                if (l == Level::Scalar) scalarNs = ns;
                char cell[64];
                std::snprintf(cell, sizeof(cell), "%.3f (%.2fx)", ns, scalarNs / ns);
                std::printf(" %18s", cell);
            }

            std::printf("\n");
        }
    }
}

int main(int argc, char **argv) {
    std::printf("RR64 SIMD harness: build %s, CPU level %s\n", RR64Simd::baselineName(), RR64Simd::levelName(RR64Simd::level()));
    std::printf("Levels under test:");
    for (Level l : levels()) std::printf(" %s", RR64Simd::levelName(l));
    std::printf("\n");

    struct Test { const char *name; void (*fn)(); };
    const Test tests[] = {
        { "vertex attributes", testVertexAttributes },
        { "vertex transform", testTransform },
        { "texcoords (exhaustive s/t)", testTexcoords },
        { "geometry scans", testGeometryScans },
        { "CI8 palette presence", testPalettePresence },
        { "TMEM word-pair swizzles", testTMEMSwizzle },
        { "TMEM texel runs / loads", testTMEMRuns },
    };

    for (const Test &t : tests) {
        const long before = failures, cmpBefore = comparisons;
        t.fn();
        std::printf("  %-30s %8ld comparisons  %s\n", t.name, comparisons - cmpBefore, (failures == before) ? "PASS" : "FAIL");
    }

    std::printf("%s: %ld comparisons, %ld mismatches\n", failures ? "FAILED" : "PASSED", comparisons, failures);
    if ((argc > 1) && (std::strcmp(argv[1], "--bench") == 0)) {
        bench();
    }

    RR64Simd::forceLevelForTesting(-1);
    return failures ? 1 : 0;
}
