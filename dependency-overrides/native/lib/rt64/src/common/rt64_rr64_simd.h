//
// RT64 - RR64 SIMD kernels
//
// RR64-SIMD: one dispatch point for the hand-vectorized hot paths in the
// renderer front end (RSP vertex packets, geometry certification scans and
// the TMEM palette scan). Every kernel has a scalar reference that is the
// original loop, and every vector path is bit-exact against that reference
// for every input the reference accepts; tests/simd/rr64_simd_harness.cpp
// enforces this on each ISA. Build floors and dispatch:
//
//   x86-64  AVX floor (compile-time), AVX2 selected at run time when the CPU
//           and OS support it. RR64_X86_BASELINE=AVX2 raises the floor.
//   arm64   NEON (always present on Apple Silicon).
//
// RR64_SIMD=scalar|vec128|avx|avx2 in the environment caps the selected level
// so a player or developer can A/B a suspected regression without a rebuild.
// The cap never raises the level above what the CPU supports.
// RR64_SIMD_VERIFY=1 additionally checks every kernel call against the scalar
// reference during real play (slow; for validation sessions only).
//

#pragma once

#include <cstddef>
#include <cstdint>

#if defined(_M_X64) || defined(__x86_64__)
#   define RR64_SIMD_X64 1
#   include <immintrin.h>
#elif defined(_M_ARM64) || defined(__aarch64__)
#   define RR64_SIMD_ARM64 1
#   include <arm_neon.h>
#endif

namespace RT64 {
    namespace RR64Simd {
        enum class Level : uint32_t {
            Scalar = 0,
            Vec128 = 1, // SSE4.1 (VEX-encoded under the AVX floor) or NEON.
            AVX = 2,    // 256-bit float lanes.
            AVX2 = 3    // 256-bit integer lanes.
        };

        // Detected CPU level, capped by RR64_SIMD. Resolved once, thread-safe.
        Level level();

        // Human-readable names for logs and the startup banner.
        const char *levelName(Level level);
        const char *baselineName();

        // Raw CPU capability, ignoring the RR64_SIMD cap.
        bool cpuHasAVX();
        bool cpuHasAVX2();

        // ---------------------------------------------------------------
        // RSP vertex packets (RT64::RSP::setVertexCommon)
        // ---------------------------------------------------------------
        //
        // `vertices` points at RSP::Vertex records, 16 bytes each, already
        // word-swapped into host order by the RDRAM memcpy:
        //   +0 int16 y, +2 int16 x, +4 uint16 flag, +6 int16 z,
        //   +8 int16 t, +10 int16 s, +12 a, +13 b, +14 g, +15 r.
        // rt64_rsp.cpp static_asserts this layout at the call site.

        // posOut[j*3..] = {x, y, z}; normColOut[j*4..] = {r, g, b, fog ? 0 : a}.
        void convertVertexAttributes(const uint8_t *vertices, uint32_t count, bool suppressFog,
            float *posOut, uint8_t *normColOut);

        // transformedOut[j] = mul(float4(pos, 1), mvp) and
        // screenOut[j].xyz = (t.xyz / float3(t.w, -t.w, t.w)) * scale + translate,
        // reproducing hlslpp's operation order for the active build so both
        // 16-byte records (hlslpp float4 / float3, padding lane included) are
        // bit-identical to the hlslpp loop it replaces. The one exception is a
        // NaN result from a degenerate matrix: it is NaN on every path, but its
        // sign/payload is compiler-defined even for hlslpp (operand commuting,
        // x / -w folded to -(x / w)). mvp is the row-major 4x4 (16 floats);
        // scale/translate are the 4-float hlslpp::float3 storage.
        void transformVertices(const float *pos, uint32_t count, const float *mvp,
            const float *scale, const float *translate, float *transformedOut, float *screenOut);

        // tcOut[j*2..] = {float(double(s * sc) / 2^21), float(double(t * tc) / 2^21)}.
        void computeTexcoords(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut);

        // ---------------------------------------------------------------
        // Geometry certification (rt64_game_frame.cpp / geometry_match.h)
        // ---------------------------------------------------------------

        // Writes bit patterns with both zeros canonicalized to +0. Returns false
        // on the first non-finite value; dst is then unspecified (the caller
        // discards the mesh), matching the original loop's early break.
        bool canonicalizePositionBits(const float *src, uint32_t *dst, size_t count);

        // True when every value is finite.
        bool allFinite(const float *values, size_t count);

        // True when every index i satisfies first <= i and i - first < vertexCount,
        // evaluated in 64-bit exactly like RR64GeometryMatch::validGeometry.
        bool indicesInRange(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount);

        // True when a[i] == b[i] numerically for all i (signed zeros compare equal).
        // Inputs are finite; NaN would compare unequal exactly as the scalar loop.
        bool floatsEqual(const float *a, const float *b, size_t count);

        // True when (a[i] - firstA) == (b[i] - firstB) in uint32 arithmetic for all i.
        bool offsetIndicesEqual(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB);

        // ---------------------------------------------------------------
        // TMEM palette scan (TMEMHasher version 5, CI8)
        // ---------------------------------------------------------------

        // Marks seen[b] = 1 for every byte b in data. Store-only so consecutive
        // indices never serialize on a read-modify-write of the same word.
        void markBytesPresent(const uint8_t *data, size_t count, uint8_t *seen /* 256 */);

        // ORs the 256-entry presence table into four 64-bit words (bit b of
        // word b>>6), the layout TMEMHasher consumes.
        void packBytesPresent(const uint8_t *seen /* 256 */, uint64_t *bits /* 4 */);

        // ---------------------------------------------------------------
        // TMEM load word pairs (RDP::loadToTMEMCommon fast paths)
        // ---------------------------------------------------------------
        //
        // Inline so they stay inside the per-word load loop. Both replace the
        // former SSE2-only sequences (x86 only, 4-5 shuffles each) with one
        // byte shuffle, and give arm64 the same two-words-per-step path.

#if defined(RR64_SIMD_X64) || defined(RR64_SIMD_ARM64)
#   define RR64_SIMD_HAS_TMEM_SWIZZLE 1

        // Two TLUT palette entries from four aligned RDRAM bytes {s0..s3} to two
        // eight-byte TMEM words: {s3 s2} x4 then {s1 s0} x4.
        inline void tmemPaletteWordPair(uint32_t source, uint8_t *dst) {
#   if defined(RR64_SIMD_X64)
            const __m128i order = _mm_setr_epi8(3, 2, 3, 2, 3, 2, 3, 2, 1, 0, 1, 0, 1, 0, 1, 0);
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_shuffle_epi8(_mm_cvtsi32_si128(int32_t(source)), order));
#   else
            static const uint8_t orderData[16] = { 3, 2, 3, 2, 3, 2, 3, 2, 1, 0, 1, 0, 1, 0, 1, 0 };
            vst1q_u8(dst, vqtbl1q_u8(vreinterpretq_u8_u32(vdupq_n_u32(source)), vld1q_u8(orderData)));
#   endif
        }

        // Two eight-byte texel words from RDRAM (word-swapped storage) to TMEM:
        // bytes reversed within each 32-bit word, and the 32-bit halves of each
        // 64-bit word exchanged on odd rows (TMEM's tmemXorMask == 4).
        inline void tmemTexelWordPair(const uint8_t *src, uint8_t *dst, bool oddRowSwap) {
#   if defined(RR64_SIMD_X64)
            const __m128i plain = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
            const __m128i swapped = _mm_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
            const __m128i bytes = _mm_loadu_si128(reinterpret_cast<const __m128i *>(src));
            _mm_storeu_si128(reinterpret_cast<__m128i *>(dst), _mm_shuffle_epi8(bytes, oddRowSwap ? swapped : plain));
#   else
            const uint8x16_t bytes = vld1q_u8(src);
            vst1q_u8(dst, oddRowSwap ? vrev64q_u8(bytes) : vrev32q_u8(bytes));
#   endif
        }

        // A run of `words` texel words (any count >= 1) with the same byte
        // mapping as tmemTexelWordPair applied to every word: 32 bytes per
        // step with AVX2, 16 with SSE4.1/NEON, one bswap pair for an odd last
        // word. Reads exactly words * 8 source bytes and writes exactly
        // words * 8 destination bytes. Tuning pass 2: lets a whole contiguous
        // TMEM row segment skip the per-word step logic in loadToTMEMCommon.
        void tmemTexelWords(const uint8_t *src, uint8_t *dst, uint32_t words, bool oddRowSwap);
#endif

        // ---------------------------------------------------------------
        // Testing
        // ---------------------------------------------------------------

        // Forces level() for the correctness harness (clamped to what the CPU
        // supports). Pass -1 to return to the detected/capped level.
        void forceLevelForTesting(int level);

        // RR64_SIMD_VERIFY=1 shadows every out-of-line kernel call with the
        // scalar reference on the same live data and reports differences on
        // stderr. These counters let the application log a summary at exit.
        uint64_t verifyMismatches();
        uint64_t verifyCallCount();
    };
};
