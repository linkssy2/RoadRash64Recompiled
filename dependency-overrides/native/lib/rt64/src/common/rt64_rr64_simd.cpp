//
// RT64 - RR64 SIMD kernels
//
// See rt64_rr64_simd.h for the contract. Conventions used throughout:
//
//  * The scalar functions are the original loops, kept verbatim in meaning.
//    They are the reference the harness compares against and the path taken
//    when RR64_SIMD=scalar.
//  * x86 "Vec128" kernels use SSE4.1 intrinsics. The project floor is AVX, so
//    the compiler emits them VEX-encoded; they serve the 4-wide remainders of
//    the 256-bit loops and the vec128 cap.
//  * AVX kernels only use 256-bit float operations (AVX1). Anything needing
//    256-bit integer lanes is AVX2 and is compiled with a function-level
//    target attribute, so the AVX floor binary still runs on Ivy Bridge
//    (MacPro6,1) and Sandy Bridge class CPUs.
//  * No kernel uses FMA unless the whole build does (__FMA__), because the
//    hlslpp code being replaced only fuses under that same condition.
//

#include "rt64_rr64_simd.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

// Pull in hlslpp so the transform kernels see exactly the configuration
// (256-bit registers, FMA) that the replaced hlslpp code is compiled with.
#include "hlsl++.h"

#if defined(RR64_SIMD_X64)
#   if defined(_MSC_VER)
#       include <intrin.h>
#   else
#       include <cpuid.h>
#   endif
#   include <immintrin.h>
#elif defined(RR64_SIMD_ARM64)
#   include <arm_neon.h>
#endif

#if defined(RR64_SIMD_X64) && (defined(__clang__) || defined(__GNUC__))
#   define RR64_TARGET_AVX2 __attribute__((target("avx2")))
#else
#   define RR64_TARGET_AVX2
#endif

#if defined(RR64_SIMD_X64) && !defined(__AVX__)
#   error "RR64 x86-64 builds require the AVX floor (-mavx or /arch:AVX). See native/cmake/Simd.cmake."
#endif

namespace RT64 {
    namespace RR64Simd {
        // ---------------------------------------------------------------
        // CPU detection
        // ---------------------------------------------------------------

        namespace {
            struct CpuInfo {
                bool sse41 = false;
                bool avx = false;
                bool avx2 = false;
            };

#if defined(RR64_SIMD_X64)
            void cpuid(uint32_t leaf, uint32_t subleaf, uint32_t regs[4]) {
#   if defined(_MSC_VER)
                int r[4];
                __cpuidex(r, int(leaf), int(subleaf));
                for (int i = 0; i < 4; i++) {
                    regs[i] = uint32_t(r[i]);
                }
#   else
                __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#   endif
            }

            uint64_t xgetbv0() {
#   if defined(_MSC_VER) && !defined(__clang__)
                return _xgetbv(0);
#   else
                // Inline asm keeps this callable without the xsave target feature.
                uint32_t eax = 0, edx = 0;
                __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
                return (uint64_t(edx) << 32) | eax;
#   endif
            }
#endif

            CpuInfo detectCpu() {
                CpuInfo info;
#if defined(RR64_SIMD_X64)
                uint32_t r[4] = {};
                cpuid(0, 0, r);
                const uint32_t maxLeaf = r[0];
                cpuid(1, 0, r);
                info.sse41 = (r[2] & (1u << 19)) != 0;
                const bool osxsave = (r[2] & (1u << 27)) != 0;
                const bool avxBit = (r[2] & (1u << 28)) != 0;
                // The OS must save YMM state (XCR0 bits 1 and 2) or AVX
                // instructions fault even when CPUID advertises them.
                const bool ymmState = osxsave && ((xgetbv0() & 0x6) == 0x6);
                info.avx = avxBit && ymmState;
                if (maxLeaf >= 7) {
                    cpuid(7, 0, r);
                    info.avx2 = info.avx && ((r[1] & (1u << 5)) != 0);
                }
#endif
                return info;
            }

            const CpuInfo &cpu() {
                static const CpuInfo info = detectCpu();
                return info;
            }

            Level detectLevel() {
#if defined(RR64_SIMD_X64)
                const CpuInfo &c = cpu();
                if (c.avx2) {
                    return Level::AVX2;
                }
                if (c.avx) {
                    return Level::AVX;
                }
                // Unreachable in a shipped build (the startup check refuses
                // non-AVX CPUs), but keep the ladder honest.
                return c.sse41 ? Level::Vec128 : Level::Scalar;
#elif defined(RR64_SIMD_ARM64)
                return Level::Vec128;
#else
                return Level::Scalar;
#endif
            }

            Level resolveLevel() {
                Level detected = detectLevel();
                const char *cap = std::getenv("RR64_SIMD");
                if (cap == nullptr || cap[0] == '\0') {
                    return detected;
                }

                Level limit = detected;
                if (std::strcmp(cap, "scalar") == 0) {
                    limit = Level::Scalar;
                }
                else if ((std::strcmp(cap, "vec128") == 0) || (std::strcmp(cap, "sse4") == 0) || (std::strcmp(cap, "neon") == 0)) {
                    limit = Level::Vec128;
                }
                else if (std::strcmp(cap, "avx") == 0) {
                    limit = Level::AVX;
                }
                else if (std::strcmp(cap, "avx2") == 0) {
                    limit = Level::AVX2;
                }

                return std::min(detected, limit);
            }
        }

        namespace {
            std::atomic<int> forcedLevel{ -1 };
        }

        Level level() {
            const int forced = forcedLevel.load(std::memory_order_relaxed);
            if (forced >= 0) {
                return Level(forced);
            }

            static const Level resolved = resolveLevel();
            return resolved;
        }

        void forceLevelForTesting(int l) {
            if (l < 0) {
                forcedLevel.store(-1, std::memory_order_relaxed);
                return;
            }

            forcedLevel.store(int(std::min(Level(l), detectLevel())), std::memory_order_relaxed);
        }

        const char *levelName(Level l) {
            switch (l) {
            case Level::Scalar:
                return "scalar";
            case Level::Vec128:
#if defined(RR64_SIMD_ARM64)
                return "NEON";
#else
                return "SSE4.1 (128-bit)";
#endif
            case Level::AVX:
                return "AVX";
            case Level::AVX2:
                return "AVX2";
            default:
                return "unknown";
            }
        }

        const char *baselineName() {
#if defined(RR64_SIMD_X64) && defined(__AVX2__)
            return "x86-64 AVX2 floor";
#elif defined(RR64_SIMD_X64)
            return "x86-64 AVX floor, AVX2 dispatched";
#elif defined(RR64_SIMD_ARM64)
            return "arm64 NEON";
#else
            return "generic";
#endif
        }

        bool cpuHasAVX() {
            return cpu().avx;
        }

        bool cpuHasAVX2() {
            return cpu().avx2;
        }

        // ---------------------------------------------------------------
        // Scalar references
        // ---------------------------------------------------------------

        namespace scalar {
            inline int16_t readS16(const uint8_t *p) {
                int16_t v;
                std::memcpy(&v, p, sizeof(v));
                return v;
            }

            void convertVertexAttributes(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
                for (uint32_t j = 0; j < count; j++) {
                    const uint8_t *v = vertices + size_t(j) * 16;
                    posOut[j * 3 + 0] = readS16(v + 2);
                    posOut[j * 3 + 1] = readS16(v + 0);
                    posOut[j * 3 + 2] = readS16(v + 6);
                    normColOut[j * 4 + 0] = v[15];
                    normColOut[j * 4 + 1] = v[14];
                    normColOut[j * 4 + 2] = v[13];
                    normColOut[j * 4 + 3] = suppressFog ? 0 : v[12];
                }
            }

            // The original RT64/RR64 loop, operating on real hlslpp objects.
            void transformVertices(const float *pos, uint32_t count, const float *mvpFloats, const float *scaleFloats, const float *translateFloats, float *transformedOut, float *screenOut) {
                hlslpp::float4x4 mvp;
                hlslpp::float3 scale, translate;
                // The callers pass the storage of real hlslpp objects, so a byte
                // copy rebuilds them exactly (including the float3 padding lane).
                std::memcpy(static_cast<void *>(&mvp), mvpFloats, sizeof(mvp));
                std::memcpy(static_cast<void *>(&scale), scaleFloats, sizeof(scale));
                std::memcpy(static_cast<void *>(&translate), translateFloats, sizeof(translate));
                hlslpp::float4 *transformedOutput = reinterpret_cast<hlslpp::float4 *>(transformedOut);
                hlslpp::float3 *screenOutput = reinterpret_cast<hlslpp::float3 *>(screenOut);
                uint32_t floatIndex = 0;
                uint32_t i = 0;
                for (; i + 1 < count; i += 2) {
                    const hlslpp::float4 a = hlslpp::mul(hlslpp::float4(pos[floatIndex], pos[floatIndex + 1], pos[floatIndex + 2], 1.0f), mvp);
                    const hlslpp::float4 b = hlslpp::mul(hlslpp::float4(pos[floatIndex + 3], pos[floatIndex + 4], pos[floatIndex + 5], 1.0f), mvp);
                    transformedOutput[0] = a;
                    transformedOutput[1] = b;
                    screenOutput[0] = (a.xyz / hlslpp::float3(a.w, -a.w, a.w)) * scale + translate;
                    screenOutput[1] = (b.xyz / hlslpp::float3(b.w, -b.w, b.w)) * scale + translate;
                    transformedOutput += 2;
                    screenOutput += 2;
                    floatIndex += 6;
                }

                for (; i < count; i++) {
                    const hlslpp::float4 tfPos = hlslpp::mul(hlslpp::float4(pos[floatIndex + 0], pos[floatIndex + 1], pos[floatIndex + 2], 1.0f), mvp);
                    *transformedOutput++ = tfPos;
                    *screenOutput++ = (tfPos.xyz / hlslpp::float3(tfPos.w, -tfPos.w, tfPos.w)) * scale + translate;
                    floatIndex += 3;
                }
            }

            void computeTexcoords(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
                const double Divisor = 65536.0f * 32.0f;
                for (uint32_t j = 0; j < count; j++) {
                    const uint8_t *v = vertices + size_t(j) * 16;
                    const int16_t s = readS16(v + 10);
                    const int16_t t = readS16(v + 8);
                    tcOut[j * 2 + 0] = (float)((double)(s * sc) / Divisor);
                    tcOut[j * 2 + 1] = (float)((double)(t * tc) / Divisor);
                }
            }

            bool canonicalizePositionBits(const float *src, uint32_t *dst, size_t count) {
                for (size_t i = 0; i < count; i++) {
                    if (!std::isfinite(src[i])) {
                        return false;
                    }

                    uint32_t bits = 0;
                    if (src[i] != 0.0f) {
                        std::memcpy(&bits, &src[i], sizeof(bits));
                    }

                    dst[i] = bits;
                }

                return true;
            }

            bool allFinite(const float *values, size_t count) {
                for (size_t i = 0; i < count; i++) {
                    if (!std::isfinite(values[i])) {
                        return false;
                    }
                }

                return true;
            }

            bool indicesInRange(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
                for (size_t i = 0; i < count; i++) {
                    const uint32_t index = indices[i];
                    if ((index < first) || (uint64_t(index) - uint64_t(first) >= vertexCount)) {
                        return false;
                    }
                }

                return true;
            }

            bool floatsEqual(const float *a, const float *b, size_t count) {
                for (size_t i = 0; i < count; i++) {
                    if (a[i] != b[i]) {
                        return false;
                    }
                }

                return true;
            }

            bool offsetIndicesEqual(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
                for (size_t i = 0; i < count; i++) {
                    if ((a[i] - firstA) != (b[i] - firstB)) {
                        return false;
                    }
                }

                return true;
            }

            void packBytesPresent(const uint8_t *seen, uint64_t *bits) {
                for (uint32_t i = 0; i < 256; i++) {
                    if (seen[i] != 0) {
                        bits[i >> 6] |= (1ULL << (i & 0x3F));
                    }
                }
            }
        }

        // ---------------------------------------------------------------
        // x86-64
        // ---------------------------------------------------------------

#if defined(RR64_SIMD_X64)
        namespace x86 {
            // hlslpp's madd is fused exactly when __FMA__ is defined (hlslpp
            // defines it itself for MSVC /arch:AVX2); mirror that decision.
#   if defined(__FMA__)
#       define RR64_MADD128(a, b, c) _mm_fmadd_ps((a), (b), (c))
#       define RR64_MADD256(a, b, c) _mm256_fmadd_ps((a), (b), (c))
#   else
#       define RR64_MADD128(a, b, c) _mm_add_ps(_mm_mul_ps((a), (b)), (c))
#       define RR64_MADD256(a, b, c) _mm256_add_ps(_mm256_mul_ps((a), (b)), (c))
#   endif

            // Four 16-byte vertex records transposed to dword columns:
            // d0 = {y|x<<16}, d1 = {flag|z<<16}, d2 = {t|s<<16}, d3 = {a,b,g,r}.
            inline void loadVertexColumns4(const uint8_t *v, __m128i &d0, __m128i &d1, __m128i &d2, __m128i &d3) {
                __m128 r0 = _mm_loadu_ps(reinterpret_cast<const float *>(v + 0));
                __m128 r1 = _mm_loadu_ps(reinterpret_cast<const float *>(v + 16));
                __m128 r2 = _mm_loadu_ps(reinterpret_cast<const float *>(v + 32));
                __m128 r3 = _mm_loadu_ps(reinterpret_cast<const float *>(v + 48));
                _MM_TRANSPOSE4_PS(r0, r1, r2, r3);
                d0 = _mm_castps_si128(r0);
                d1 = _mm_castps_si128(r1);
                d2 = _mm_castps_si128(r2);
                d3 = _mm_castps_si128(r3);
            }

            inline __m128i highS16(__m128i d) {
                return _mm_srai_epi32(d, 16);
            }

            inline __m128i lowS16(__m128i d) {
                return _mm_srai_epi32(_mm_slli_epi32(d, 16), 16);
            }

            // Record-wise attribute conversion (tuning pass 2). Each 16-byte
            // record {y, x, flag, z, t, s, a, b, g, r} goes through one byte
            // shuffle that lands x, y, z (and z again) in the high halves of
            // four dwords; an arithmetic shift sign-extends them and one convert
            // gives the floats. The 16-byte store per vertex overlaps the next
            // vertex's x by one float, which the next store overwrites, so the
            // xyz stream needs no transpose or blend. The last vertex of a call
            // never takes this path (its store would pass the end of posOut).
            // Colors: record k's {a,b,g,r} is reversed into dword k of its
            // register and the four registers are ORed into one store.
            // Exact: int16 -> int32 -> float is exact either way, and the color
            // bytes are only moved.
            //
            // The final vertex of a call stores exactly 12 bytes instead
            // (storeXYZLast), so packets that are a multiple of 4/8 never fall
            // back to the scalar loop for their last group.
            inline void storeXYZLast(float *p, __m128 v) {
                _mm_storel_pi(reinterpret_cast<__m64 *>(p), v);
                _mm_store_ss(p + 2, _mm_movehl_ps(v, v));
            }

            inline __m128i positionShuffle128() {
                return _mm_setr_epi8(-1, -1, 2, 3, -1, -1, 0, 1, -1, -1, 6, 7, -1, -1, 6, 7);
            }

            inline __m128i colorShuffle128(int k) {
                alignas(16) int8_t m[16];
                for (int b = 0; b < 16; b++) {
                    m[b] = -1;
                }

                m[k * 4 + 0] = 15;
                m[k * 4 + 1] = 14;
                m[k * 4 + 2] = 13;
                m[k * 4 + 3] = 12;
                return _mm_load_si128(reinterpret_cast<const __m128i *>(m));
            }

            void convertVertexAttributes128(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
                const __m128i posShuffle = positionShuffle128();
                const __m128i c0 = colorShuffle128(0), c1 = colorShuffle128(1), c2 = colorShuffle128(2), c3 = colorShuffle128(3);
                const __m128i alphaMask = _mm_set1_epi32(suppressFog ? 0x00FFFFFF : -1);
                uint32_t j = 0;
                for (; j + 4 <= count; j += 4) {
                    const uint8_t *v = vertices + size_t(j) * 16;
                    float *p = posOut + size_t(j) * 3;
                    const __m128i r0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + 0));
                    const __m128i r1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + 16));
                    const __m128i r2 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + 32));
                    const __m128i r3 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(v + 48));
                    _mm_storeu_ps(p + 0, _mm_cvtepi32_ps(_mm_srai_epi32(_mm_shuffle_epi8(r0, posShuffle), 16)));
                    _mm_storeu_ps(p + 3, _mm_cvtepi32_ps(_mm_srai_epi32(_mm_shuffle_epi8(r1, posShuffle), 16)));
                    _mm_storeu_ps(p + 6, _mm_cvtepi32_ps(_mm_srai_epi32(_mm_shuffle_epi8(r2, posShuffle), 16)));
                    const __m128 last = _mm_cvtepi32_ps(_mm_srai_epi32(_mm_shuffle_epi8(r3, posShuffle), 16));
                    if (j + 4 < count) {
                        _mm_storeu_ps(p + 9, last);
                    }
                    else {
                        storeXYZLast(p + 9, last);
                    }
                    const __m128i colors = _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(r0, c0), _mm_shuffle_epi8(r1, c1)),
                        _mm_or_si128(_mm_shuffle_epi8(r2, c2), _mm_shuffle_epi8(r3, c3)));
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(normColOut + size_t(j) * 4), _mm_and_si128(colors, alphaMask));
                }

                scalar::convertVertexAttributes(vertices + size_t(j) * 16, count - j, suppressFog, posOut + size_t(j) * 3, normColOut + size_t(j) * 4);
            }

            // Transform layout note. RT64 stores clip and screen positions as
            // 16-byte AoS records, so the kernels below keep one vertex per
            // 128-bit lane (two per AVX register) instead of going SoA: an SoA
            // form needs two 8x4 transposes per eight vertices and measured no
            // faster than hlslpp. Per lane the operation sequence is hlslpp's own,
            // so every float of both records, including the screen record's
            // padding lane, is bit-identical to the loop being replaced.
            //
            // mul(float4(x, y, z, 1), M): with hlslpp's 128-bit registers (this
            // project pins them, HLSLPP_NO_SIMD_REGISTER_256) the sum is
            // ((x*M0 + y*M1) + z*M2) + 1*M3. Should hlslpp ever run with 256-bit
            // registers, rows 0|1 and 2|3 share a register and the sum becomes
            // (z*M2 + x*M0) + (1*M3 + y*M1); both orders are kept in sync here.
            inline __m128 transformRow128(__m128 X, __m128 Y, __m128 Z, __m128 one, __m128 R0, __m128 R1, __m128 R2, __m128 R3) {
#   if defined(HLSLPP_SIMD_REGISTER_256)
                const __m128 lo = RR64_MADD128(R2, Z, _mm_mul_ps(R0, X));
                const __m128 hi = RR64_MADD128(R3, one, _mm_mul_ps(R1, Y));
                return _mm_add_ps(lo, hi);
#   else
                return RR64_MADD128(R3, one, RR64_MADD128(R2, Z, RR64_MADD128(R1, Y, _mm_mul_ps(R0, X))));
#   endif
            }

            inline __m256 transformRow256(__m256 X, __m256 Y, __m256 Z, __m256 one, __m256 R0, __m256 R1, __m256 R2, __m256 R3) {
#   if defined(HLSLPP_SIMD_REGISTER_256)
                const __m256 lo = RR64_MADD256(R2, Z, _mm256_mul_ps(R0, X));
                const __m256 hi = RR64_MADD256(R3, one, _mm256_mul_ps(R1, Y));
                return _mm256_add_ps(lo, hi);
#   else
                return RR64_MADD256(R3, one, RR64_MADD256(R2, Z, RR64_MADD256(R1, Y, _mm256_mul_ps(R0, X))));
#   endif
            }

            // hlslpp divides (x, y, z, w) by float3(w, -w, w), whose padding lane
            // holds -w: the divisor is w with the sign of lanes 1 and 3 flipped.
            inline __m128 negateYW128() {
                return _mm_castsi128_ps(_mm_setr_epi32(0, int32_t(0x80000000u), 0, int32_t(0x80000000u)));
            }

            void transformVertices128(const float *pos, uint32_t count, const float *mvp, const float *scale, const float *translate, float *transformedOut, float *screenOut) {
                const __m128 R0 = _mm_loadu_ps(mvp + 0), R1 = _mm_loadu_ps(mvp + 4), R2 = _mm_loadu_ps(mvp + 8), R3 = _mm_loadu_ps(mvp + 12);
                const __m128 S = _mm_loadu_ps(scale), T = _mm_loadu_ps(translate);
                const __m128 one = _mm_set1_ps(1.0f);
                const __m128 signYW = negateYW128();
                uint32_t j = 0;
                // The 4-float load at p reads one float past the vertex, so the
                // last vertex of the stream goes through the scalar path.
                for (; j + 1 < count; j++) {
                    const __m128 v = _mm_loadu_ps(pos + size_t(j) * 3);
                    const __m128 o = transformRow128(_mm_shuffle_ps(v, v, 0x00), _mm_shuffle_ps(v, v, 0x55), _mm_shuffle_ps(v, v, 0xAA), one, R0, R1, R2, R3);
                    const __m128 w = _mm_shuffle_ps(o, o, 0xFF);
                    _mm_storeu_ps(transformedOut + size_t(j) * 4, o);
                    _mm_storeu_ps(screenOut + size_t(j) * 4, _mm_add_ps(_mm_mul_ps(_mm_div_ps(o, _mm_xor_ps(w, signYW)), S), T));
                }

                scalar::transformVertices(pos + size_t(j) * 3, count - j, mvp, scale, translate, transformedOut + size_t(j) * 4, screenOut + size_t(j) * 4);
            }

            void transformVerticesAVX(const float *pos, uint32_t count, const float *mvp, const float *scale, const float *translate, float *transformedOut, float *screenOut) {
                const __m256 R0 = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(mvp + 0));
                const __m256 R1 = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(mvp + 4));
                const __m256 R2 = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(mvp + 8));
                const __m256 R3 = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(mvp + 12));
                const __m256 S = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(scale));
                const __m256 T = _mm256_broadcast_ps(reinterpret_cast<const __m128 *>(translate));
                const __m256 one = _mm256_set1_ps(1.0f);
                const __m256 signYW = _mm256_set_m128(negateYW128(), negateYW128());
                uint32_t j = 0;
                // Vertex j in the low lane, j + 1 in the high lane. The high load
                // reads one float past vertex j + 1, so stop before the last vertex.
                for (; j + 2 < count; j += 2) {
                    const float *p = pos + size_t(j) * 3;
                    const __m256 v = _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(p)), _mm_loadu_ps(p + 3), 1);
                    const __m256 o = transformRow256(_mm256_permute_ps(v, 0x00), _mm256_permute_ps(v, 0x55), _mm256_permute_ps(v, 0xAA), one, R0, R1, R2, R3);
                    const __m256 w = _mm256_permute_ps(o, 0xFF);
                    const __m256 screen = _mm256_add_ps(_mm256_mul_ps(_mm256_div_ps(o, _mm256_xor_ps(w, signYW)), S), T);
                    _mm256_storeu_ps(transformedOut + size_t(j) * 4, o);
                    _mm256_storeu_ps(screenOut + size_t(j) * 4, screen);
                }

                transformVertices128(pos + size_t(j) * 3, count - j, mvp, scale, translate, transformedOut + size_t(j) * 4, screenOut + size_t(j) * 4);
            }

            void computeTexcoords128(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
                // (float)(double(p) / 2^21) == (float)p * 2^-21 for every int32 p:
                // the double holds p exactly, the power-of-two scale is exact in
                // both widths (no overflow or underflow is reachable), and both
                // sides round p to float with round-to-nearest-even once.
                const __m128i scale = _mm_setr_epi32(sc, tc, sc, tc);
                const __m128 inv = _mm_set1_ps(1.0f / 2097152.0f);
                uint32_t j = 0;
                for (; j + 4 <= count; j += 4) {
                    __m128i d0, d1, d2, d3;
                    loadVertexColumns4(vertices + size_t(j) * 16, d0, d1, d2, d3);
                    const __m128i s = highS16(d2);
                    const __m128i t = lowS16(d2);
                    const __m128i st01 = _mm_unpacklo_epi32(s, t); // s0 t0 s1 t1
                    const __m128i st23 = _mm_unpackhi_epi32(s, t); // s2 t2 s3 t3
                    _mm_storeu_ps(tcOut + size_t(j) * 2 + 0, _mm_mul_ps(_mm_cvtepi32_ps(_mm_mullo_epi32(st01, scale)), inv));
                    _mm_storeu_ps(tcOut + size_t(j) * 2 + 4, _mm_mul_ps(_mm_cvtepi32_ps(_mm_mullo_epi32(st23, scale)), inv));
                }

                scalar::computeTexcoords(vertices + size_t(j) * 16, count - j, sc, tc, tcOut + size_t(j) * 2);
            }

            // AVX2 versions of the integer-lane packet kernels. Eight records
            // are loaded as lane pairs (v0|v4 ... v3|v7) so one in-lane 4x4
            // transpose produces dword columns for vertices 0-3 | 4-7.
            inline __m256 loadRecordPair(const uint8_t *v, int a) {
                return _mm256_insertf128_ps(_mm256_castps128_ps256(_mm_loadu_ps(reinterpret_cast<const float *>(v + a * 16))),
                    _mm_loadu_ps(reinterpret_cast<const float *>(v + (a + 4) * 16)), 1);
            }

            RR64_TARGET_AVX2 inline void loadVertexColumns8(const uint8_t *v, __m256i &d0, __m256i &d1, __m256i &d2, __m256i &d3) {
                const __m256 r0 = loadRecordPair(v, 0), r1 = loadRecordPair(v, 1), r2 = loadRecordPair(v, 2), r3 = loadRecordPair(v, 3);
                const __m256 t0 = _mm256_unpacklo_ps(r0, r1);
                const __m256 t1 = _mm256_unpackhi_ps(r0, r1);
                const __m256 t2 = _mm256_unpacklo_ps(r2, r3);
                const __m256 t3 = _mm256_unpackhi_ps(r2, r3);
                d0 = _mm256_castps_si256(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(1, 0, 1, 0)));
                d1 = _mm256_castps_si256(_mm256_shuffle_ps(t0, t2, _MM_SHUFFLE(3, 2, 3, 2)));
                d2 = _mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(1, 0, 1, 0)));
                d3 = _mm256_castps_si256(_mm256_shuffle_ps(t1, t3, _MM_SHUFFLE(3, 2, 3, 2)));
            }

            // AVX2: two records per register (consecutive 32 bytes), same
            // per-record shuffle in both lanes. Lane 0 of register k holds
            // vertex 2k, lane 1 vertex 2k + 1; stores go out in vertex order so
            // each overlapping store is overwritten by its successor. Colors:
            // register k puts both of its colors in dword k of each lane, the
            // ORed result holds {v0 v2 v4 v6 | v1 v3 v5 v7}, and one cross-lane
            // dword permute restores v0..v7.
            RR64_TARGET_AVX2 void convertVertexAttributesAVX2(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
                const __m256i posShuffle = _mm256_broadcastsi128_si256(positionShuffle128());
                const __m256i c0 = _mm256_broadcastsi128_si256(colorShuffle128(0)), c1 = _mm256_broadcastsi128_si256(colorShuffle128(1));
                const __m256i c2 = _mm256_broadcastsi128_si256(colorShuffle128(2)), c3 = _mm256_broadcastsi128_si256(colorShuffle128(3));
                const __m256i colorOrder = _mm256_setr_epi32(0, 4, 1, 5, 2, 6, 3, 7);
                const __m256i alphaMask = _mm256_set1_epi32(suppressFog ? 0x00FFFFFF : -1);
                uint32_t j = 0;
                for (; j + 8 <= count; j += 8) {
                    const uint8_t *v = vertices + size_t(j) * 16;
                    float *p = posOut + size_t(j) * 3;
                    const __m256i r0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + 0));
                    const __m256i r1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + 32));
                    const __m256i r2 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + 64));
                    const __m256i r3 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(v + 96));
                    const __m256 p0 = _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_shuffle_epi8(r0, posShuffle), 16));
                    const __m256 p1 = _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_shuffle_epi8(r1, posShuffle), 16));
                    const __m256 p2 = _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_shuffle_epi8(r2, posShuffle), 16));
                    const __m256 p3 = _mm256_cvtepi32_ps(_mm256_srai_epi32(_mm256_shuffle_epi8(r3, posShuffle), 16));
                    _mm_storeu_ps(p + 0, _mm256_castps256_ps128(p0));
                    _mm_storeu_ps(p + 3, _mm256_extractf128_ps(p0, 1));
                    _mm_storeu_ps(p + 6, _mm256_castps256_ps128(p1));
                    _mm_storeu_ps(p + 9, _mm256_extractf128_ps(p1, 1));
                    _mm_storeu_ps(p + 12, _mm256_castps256_ps128(p2));
                    _mm_storeu_ps(p + 15, _mm256_extractf128_ps(p2, 1));
                    _mm_storeu_ps(p + 18, _mm256_castps256_ps128(p3));
                    if (j + 8 < count) {
                        _mm_storeu_ps(p + 21, _mm256_extractf128_ps(p3, 1));
                    }
                    else {
                        storeXYZLast(p + 21, _mm256_extractf128_ps(p3, 1));
                    }
                    const __m256i colors = _mm256_or_si256(_mm256_or_si256(_mm256_shuffle_epi8(r0, c0), _mm256_shuffle_epi8(r1, c1)),
                        _mm256_or_si256(_mm256_shuffle_epi8(r2, c2), _mm256_shuffle_epi8(r3, c3)));
                    _mm256_storeu_si256(reinterpret_cast<__m256i *>(normColOut + size_t(j) * 4),
                        _mm256_and_si256(_mm256_permutevar8x32_epi32(colors, colorOrder), alphaMask));
                }

                convertVertexAttributes128(vertices + size_t(j) * 16, count - j, suppressFog, posOut + size_t(j) * 3, normColOut + size_t(j) * 4);
            }

            RR64_TARGET_AVX2 void computeTexcoordsAVX2(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
                const __m256i scale = _mm256_setr_epi32(sc, tc, sc, tc, sc, tc, sc, tc);
                const __m256 inv = _mm256_set1_ps(1.0f / 2097152.0f);
                uint32_t j = 0;
                for (; j + 8 <= count; j += 8) {
                    __m256i d0, d1, d2, d3;
                    loadVertexColumns8(vertices + size_t(j) * 16, d0, d1, d2, d3);
                    const __m256i s = _mm256_srai_epi32(d2, 16);
                    const __m256i t = _mm256_srai_epi32(_mm256_slli_epi32(d2, 16), 16);
                    const __m256i stA = _mm256_unpacklo_epi32(s, t); // s0 t0 s1 t1 | s4 t4 s5 t5
                    const __m256i stB = _mm256_unpackhi_epi32(s, t); // s2 t2 s3 t3 | s6 t6 s7 t7
                    const __m256 fA = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(stA, scale)), inv);
                    const __m256 fB = _mm256_mul_ps(_mm256_cvtepi32_ps(_mm256_mullo_epi32(stB, scale)), inv);
                    float *out = tcOut + size_t(j) * 2;
                    _mm256_storeu_ps(out + 0, _mm256_permute2f128_ps(fA, fB, 0x20)); // v0..v3
                    _mm256_storeu_ps(out + 8, _mm256_permute2f128_ps(fA, fB, 0x31)); // v4..v7
                }

                computeTexcoords128(vertices + size_t(j) * 16, count - j, sc, tc, tcOut + size_t(j) * 2);
            }

            // --- geometry scans -------------------------------------------

            inline __m128 finiteMask128(__m128 v) {
                const __m128 absMask = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
                const __m128 inf = _mm_set1_ps(std::numeric_limits<float>::infinity());
                return _mm_cmplt_ps(_mm_and_ps(v, absMask), inf); // NaN compares false
            }

            inline __m256 finiteMask256(__m256 v) {
                const __m256 absMask = _mm256_castsi256_ps(_mm256_set1_epi32(0x7FFFFFFF));
                const __m256 inf = _mm256_set1_ps(std::numeric_limits<float>::infinity());
                return _mm256_cmp_ps(_mm256_and_ps(v, absMask), inf, _CMP_LT_OQ);
            }

            bool canonicalizePositionBits128(const float *src, uint32_t *dst, size_t count) {
                const __m128 zero = _mm_setzero_ps();
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    const __m128 v = _mm_loadu_ps(src + i);
                    if (_mm_movemask_ps(finiteMask128(v)) != 0xF) {
                        return false;
                    }

                    // Keep bits where v != 0; both signed zeros become +0.
                    _mm_storeu_ps(reinterpret_cast<float *>(dst + i), _mm_and_ps(v, _mm_cmpneq_ps(v, zero)));
                }

                return scalar::canonicalizePositionBits(src + i, dst + i, count - i);
            }

            bool canonicalizePositionBitsAVX(const float *src, uint32_t *dst, size_t count) {
                const __m256 zero = _mm256_setzero_ps();
                size_t i = 0;
                // Tuning pass 2: 16 floats per branch. On a non-finite value the
                // caller discards dst, so storing the first eight before the
                // check is harmless (the result, false, is unchanged).
                for (; i + 16 <= count; i += 16) {
                    const __m256 v0 = _mm256_loadu_ps(src + i);
                    const __m256 v1 = _mm256_loadu_ps(src + i + 8);
                    if (_mm256_movemask_ps(_mm256_and_ps(finiteMask256(v0), finiteMask256(v1))) != 0xFF) {
                        return false;
                    }

                    _mm256_storeu_ps(reinterpret_cast<float *>(dst + i), _mm256_and_ps(v0, _mm256_cmp_ps(v0, zero, _CMP_NEQ_UQ)));
                    _mm256_storeu_ps(reinterpret_cast<float *>(dst + i + 8), _mm256_and_ps(v1, _mm256_cmp_ps(v1, zero, _CMP_NEQ_UQ)));
                }

                for (; i + 8 <= count; i += 8) {
                    const __m256 v = _mm256_loadu_ps(src + i);
                    if (_mm256_movemask_ps(finiteMask256(v)) != 0xFF) {
                        return false;
                    }

                    _mm256_storeu_ps(reinterpret_cast<float *>(dst + i), _mm256_and_ps(v, _mm256_cmp_ps(v, zero, _CMP_NEQ_UQ)));
                }

                return canonicalizePositionBits128(src + i, dst + i, count - i);
            }

            bool allFinite128(const float *values, size_t count) {
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    if (_mm_movemask_ps(finiteMask128(_mm_loadu_ps(values + i))) != 0xF) {
                        return false;
                    }
                }

                return scalar::allFinite(values + i, count - i);
            }

            bool allFiniteAVX(const float *values, size_t count) {
                size_t i = 0;
                for (; i + 16 <= count; i += 16) {
                    // Two independent compares per iteration; AND them so one
                    // movemask decides both halves.
                    const __m256 a = finiteMask256(_mm256_loadu_ps(values + i));
                    const __m256 b = finiteMask256(_mm256_loadu_ps(values + i + 8));
                    if (_mm256_movemask_ps(_mm256_and_ps(a, b)) != 0xFF) {
                        return false;
                    }
                }

                return allFinite128(values + i, count - i);
            }

            bool floatsEqual128(const float *a, const float *b, size_t count) {
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    if (_mm_movemask_ps(_mm_cmpeq_ps(_mm_loadu_ps(a + i), _mm_loadu_ps(b + i))) != 0xF) {
                        return false;
                    }
                }

                return scalar::floatsEqual(a + i, b + i, count - i);
            }

            bool floatsEqualAVX(const float *a, const float *b, size_t count) {
                size_t i = 0;
                // Tuning pass 2: 16 floats per branch, like allFiniteAVX.
                for (; i + 16 <= count; i += 16) {
                    const __m256 eq0 = _mm256_cmp_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), _CMP_EQ_OQ);
                    const __m256 eq1 = _mm256_cmp_ps(_mm256_loadu_ps(a + i + 8), _mm256_loadu_ps(b + i + 8), _CMP_EQ_OQ);
                    if (_mm256_movemask_ps(_mm256_and_ps(eq0, eq1)) != 0xFF) {
                        return false;
                    }
                }

                for (; i + 8 <= count; i += 8) {
                    const __m256 eq = _mm256_cmp_ps(_mm256_loadu_ps(a + i), _mm256_loadu_ps(b + i), _CMP_EQ_OQ);
                    if (_mm256_movemask_ps(eq) != 0xFF) {
                        return false;
                    }
                }

                return floatsEqual128(a + i, b + i, count - i);
            }

            // Unsigned range test without 64-bit lanes: index >= first and
            // index - first <= vertexCount - 1 (both unsigned 32-bit). When
            // vertexCount exceeds 32 bits the second test always holds.
            bool indicesInRange128(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
                if (vertexCount == 0) {
                    return count == 0;
                }

                const uint32_t lastOffset = (vertexCount > 0xFFFFFFFFull) ? 0xFFFFFFFFu : uint32_t(vertexCount - 1);
                const __m128i vFirst = _mm_set1_epi32(int32_t(first));
                const __m128i vLast = _mm_set1_epi32(int32_t(lastOffset));
                size_t i = 0;
                // Tuning pass 2: 8 indices per branch. This is also the AVX-level
                // path (256-bit integer compares need AVX2), e.g. Ivy Bridge.
                for (; i + 8 <= count; i += 8) {
                    const __m128i index0 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(indices + i));
                    const __m128i index1 = _mm_loadu_si128(reinterpret_cast<const __m128i *>(indices + i + 4));
                    const __m128i offset0 = _mm_sub_epi32(index0, vFirst);
                    const __m128i offset1 = _mm_sub_epi32(index1, vFirst);
                    const __m128i ok0 = _mm_and_si128(_mm_cmpeq_epi32(_mm_max_epu32(index0, vFirst), index0),
                        _mm_cmpeq_epi32(_mm_min_epu32(offset0, vLast), offset0));
                    const __m128i ok1 = _mm_and_si128(_mm_cmpeq_epi32(_mm_max_epu32(index1, vFirst), index1),
                        _mm_cmpeq_epi32(_mm_min_epu32(offset1, vLast), offset1));
                    if (_mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(ok0, ok1))) != 0xF) {
                        return false;
                    }
                }

                for (; i + 4 <= count; i += 4) {
                    const __m128i index = _mm_loadu_si128(reinterpret_cast<const __m128i *>(indices + i));
                    const __m128i offset = _mm_sub_epi32(index, vFirst);
                    const __m128i aboveFirst = _mm_cmpeq_epi32(_mm_max_epu32(index, vFirst), index);
                    const __m128i belowLast = _mm_cmpeq_epi32(_mm_min_epu32(offset, vLast), offset);
                    if (_mm_movemask_ps(_mm_castsi128_ps(_mm_and_si128(aboveFirst, belowLast))) != 0xF) {
                        return false;
                    }
                }

                return scalar::indicesInRange(indices + i, count - i, first, vertexCount);
            }

            RR64_TARGET_AVX2 bool indicesInRangeAVX2(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
                if (vertexCount == 0) {
                    return count == 0;
                }

                const uint32_t lastOffset = (vertexCount > 0xFFFFFFFFull) ? 0xFFFFFFFFu : uint32_t(vertexCount - 1);
                const __m256i vFirst = _mm256_set1_epi32(int32_t(first));
                const __m256i vLast = _mm256_set1_epi32(int32_t(lastOffset));
                size_t i = 0;
                // Tuning pass 2: 16 indices per branch.
                for (; i + 16 <= count; i += 16) {
                    const __m256i index0 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(indices + i));
                    const __m256i index1 = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(indices + i + 8));
                    const __m256i offset0 = _mm256_sub_epi32(index0, vFirst);
                    const __m256i offset1 = _mm256_sub_epi32(index1, vFirst);
                    const __m256i ok0 = _mm256_and_si256(_mm256_cmpeq_epi32(_mm256_max_epu32(index0, vFirst), index0),
                        _mm256_cmpeq_epi32(_mm256_min_epu32(offset0, vLast), offset0));
                    const __m256i ok1 = _mm256_and_si256(_mm256_cmpeq_epi32(_mm256_max_epu32(index1, vFirst), index1),
                        _mm256_cmpeq_epi32(_mm256_min_epu32(offset1, vLast), offset1));
                    if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_and_si256(ok0, ok1))) != 0xFF) {
                        return false;
                    }
                }

                for (; i + 8 <= count; i += 8) {
                    const __m256i index = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(indices + i));
                    const __m256i offset = _mm256_sub_epi32(index, vFirst);
                    const __m256i aboveFirst = _mm256_cmpeq_epi32(_mm256_max_epu32(index, vFirst), index);
                    const __m256i belowLast = _mm256_cmpeq_epi32(_mm256_min_epu32(offset, vLast), offset);
                    if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_and_si256(aboveFirst, belowLast))) != 0xFF) {
                        return false;
                    }
                }

                return indicesInRange128(indices + i, count - i, first, vertexCount);
            }

            bool offsetIndicesEqual128(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
                const __m128i fa = _mm_set1_epi32(int32_t(firstA));
                const __m128i fb = _mm_set1_epi32(int32_t(firstB));
                size_t i = 0;
                // Tuning pass 2: compare a - b against firstA - firstB (equal in
                // wrapping arithmetic), 8 indices per branch. Also the AVX path.
                const __m128i delta = _mm_sub_epi32(fa, fb);
                for (; i + 8 <= count; i += 8) {
                    const __m128i d0 = _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + i)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + i)));
                    const __m128i d1 = _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + i + 4)), _mm_loadu_si128(reinterpret_cast<const __m128i *>(b + i + 4)));
                    if (_mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi32(d0, delta), _mm_cmpeq_epi32(d1, delta))) != 0xFFFF) {
                        return false;
                    }
                }

                for (; i + 4 <= count; i += 4) {
                    const __m128i da = _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(a + i)), fa);
                    const __m128i db = _mm_sub_epi32(_mm_loadu_si128(reinterpret_cast<const __m128i *>(b + i)), fb);
                    if (_mm_movemask_epi8(_mm_cmpeq_epi32(da, db)) != 0xFFFF) {
                        return false;
                    }
                }

                return scalar::offsetIndicesEqual(a + i, b + i, count - i, firstA, firstB);
            }

            RR64_TARGET_AVX2 bool offsetIndicesEqualAVX2(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
                const __m256i fa = _mm256_set1_epi32(int32_t(firstA));
                const __m256i fb = _mm256_set1_epi32(int32_t(firstB));
                size_t i = 0;
                // Tuning pass 2: (a - firstA) == (b - firstB) is
                // (a - b) == (firstA - firstB) in wrapping arithmetic, one
                // subtract per pair instead of two; 16 indices per branch.
                const __m256i delta = _mm256_sub_epi32(fa, fb);
                for (; i + 16 <= count; i += 16) {
                    const __m256i d0 = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i)), _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i)));
                    const __m256i d1 = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i + 8)), _mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i + 8)));
                    const __m256i eq = _mm256_and_si256(_mm256_cmpeq_epi32(d0, delta), _mm256_cmpeq_epi32(d1, delta));
                    if (uint32_t(_mm256_movemask_epi8(eq)) != 0xFFFFFFFFu) {
                        return false;
                    }
                }

                for (; i + 8 <= count; i += 8) {
                    const __m256i da = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(a + i)), fa);
                    const __m256i db = _mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i *>(b + i)), fb);
                    if (uint32_t(_mm256_movemask_epi8(_mm256_cmpeq_epi32(da, db))) != 0xFFFFFFFFu) {
                        return false;
                    }
                }

                return offsetIndicesEqual128(a + i, b + i, count - i, firstA, firstB);
            }

            void packBytesPresent128(const uint8_t *seen, uint64_t *bits) {
                const __m128i zero = _mm_setzero_si128();
                for (uint32_t w = 0; w < 4; w++) {
                    uint64_t word = 0;
                    for (uint32_t k = 0; k < 4; k++) {
                        const __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i *>(seen + w * 64 + k * 16));
                        const uint32_t present = uint32_t(~_mm_movemask_epi8(_mm_cmpeq_epi8(chunk, zero))) & 0xFFFFu;
                        word |= uint64_t(present) << (k * 16);
                    }

                    bits[w] |= word;
                }
            }

            RR64_TARGET_AVX2 void packBytesPresentAVX2(const uint8_t *seen, uint64_t *bits) {
                const __m256i zero = _mm256_setzero_si256();
                for (uint32_t w = 0; w < 4; w++) {
                    const __m256i lo = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(seen + w * 64));
                    const __m256i hi = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(seen + w * 64 + 32));
                    const uint64_t loBits = uint32_t(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(lo, zero)));
                    const uint64_t hiBits = uint32_t(~_mm256_movemask_epi8(_mm256_cmpeq_epi8(hi, zero)));
                    bits[w] |= loBits | (hiBits << 32);
                }
            }
        }
#endif

        // ---------------------------------------------------------------
        // arm64 NEON
        // ---------------------------------------------------------------

#if defined(RR64_SIMD_ARM64)
        namespace neon {
            // hlslpp's NEON madd is vmlaq_f32 (z + x * y). Using the same
            // intrinsic keeps contraction behavior identical to the replaced
            // code under any -ffp-contract setting.
            inline float32x4_t transformColumn(float32x4_t X, float32x4_t Y, float32x4_t Z, float32x4_t one, const float32x4_t *M, int c) {
                const float32x4_t m0 = vmulq_f32(M[0 + c], X);
                const float32x4_t m1 = vmlaq_f32(m0, M[4 + c], Y);
                const float32x4_t m2 = vmlaq_f32(m1, M[8 + c], Z);
                return vmlaq_f32(m2, M[12 + c], one);
            }

            void convertVertexAttributes(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
                const uint32x4_t alphaMask = vdupq_n_u32(suppressFog ? 0x00FFFFFFu : 0xFFFFFFFFu);
                uint32_t j = 0;
                for (; j + 4 <= count; j += 4) {
                    // LD4 de-interleaves the records into dword columns directly.
                    const uint32x4x4_t d = vld4q_u32(reinterpret_cast<const uint32_t *>(vertices + size_t(j) * 16));
                    const int32x4_t d0 = vreinterpretq_s32_u32(d.val[0]);
                    const int32x4_t d1 = vreinterpretq_s32_u32(d.val[1]);
                    float32x4x3_t xyz;
                    xyz.val[0] = vcvtq_f32_s32(vshrq_n_s32(d0, 16));
                    xyz.val[1] = vcvtq_f32_s32(vshrq_n_s32(vshlq_n_s32(d0, 16), 16));
                    xyz.val[2] = vcvtq_f32_s32(vshrq_n_s32(d1, 16));
                    vst3q_f32(posOut + size_t(j) * 3, xyz);
                    const uint8x16_t colors = vrev32q_u8(vreinterpretq_u8_u32(d.val[3]));
                    vst1q_u8(normColOut + size_t(j) * 4, vreinterpretq_u8_u32(vandq_u32(vreinterpretq_u32_u8(colors), alphaMask)));
                }

                scalar::convertVertexAttributes(vertices + size_t(j) * 16, count - j, suppressFog, posOut + size_t(j) * 3, normColOut + size_t(j) * 4);
            }

            void transformVertices(const float *pos, uint32_t count, const float *mvp, const float *scale, const float *translate, float *transformedOut, float *screenOut) {
                float32x4_t M[16];
                for (int k = 0; k < 16; k++) {
                    M[k] = vdupq_n_f32(mvp[k]);
                }

                const float32x4_t one = vdupq_n_f32(1.0f);
                const float32x4_t SX = vdupq_n_f32(scale[0]), SY = vdupq_n_f32(scale[1]), SZ = vdupq_n_f32(scale[2]), SW = vdupq_n_f32(scale[3]);
                const float32x4_t TX = vdupq_n_f32(translate[0]), TY = vdupq_n_f32(translate[1]), TZ = vdupq_n_f32(translate[2]), TW = vdupq_n_f32(translate[3]);
                uint32_t j = 0;
                // NEON goes SoA (unlike x86): LD3 de-interleaves the positions and
                // ST4 re-interleaves the records, so the transposes are free.
                for (; j + 4 <= count; j += 4) {
                    const float32x4x3_t xyz = vld3q_f32(pos + size_t(j) * 3);
                    float32x4x4_t o;
                    o.val[0] = transformColumn(xyz.val[0], xyz.val[1], xyz.val[2], one, M, 0);
                    o.val[1] = transformColumn(xyz.val[0], xyz.val[1], xyz.val[2], one, M, 1);
                    o.val[2] = transformColumn(xyz.val[0], xyz.val[1], xyz.val[2], one, M, 2);
                    o.val[3] = transformColumn(xyz.val[0], xyz.val[1], xyz.val[2], one, M, 3);
                    // hlslpp divides (x, y, z, w) by float3(w, -w, w), padding lane -w.
                    const float32x4_t negW = vnegq_f32(o.val[3]);
                    float32x4x4_t s;
                    s.val[0] = vaddq_f32(vmulq_f32(vdivq_f32(o.val[0], o.val[3]), SX), TX);
                    s.val[1] = vaddq_f32(vmulq_f32(vdivq_f32(o.val[1], negW), SY), TY);
                    s.val[2] = vaddq_f32(vmulq_f32(vdivq_f32(o.val[2], o.val[3]), SZ), TZ);
                    s.val[3] = vaddq_f32(vmulq_f32(vdivq_f32(o.val[3], negW), SW), TW);
                    // ST4 re-interleaves the four columns into 16-byte records.
                    vst4q_f32(transformedOut + size_t(j) * 4, o);
                    vst4q_f32(screenOut + size_t(j) * 4, s);
                }

                scalar::transformVertices(pos + size_t(j) * 3, count - j, mvp, scale, translate, transformedOut + size_t(j) * 4, screenOut + size_t(j) * 4);
            }

            void computeTexcoords(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
                // Same exactness argument as the x86 path: int32 -> float rounds
                // once, the power-of-two scale is exact.
                const int32x4_t vsc = vdupq_n_s32(sc);
                const int32x4_t vtc = vdupq_n_s32(tc);
                const float inv = 1.0f / 2097152.0f;
                uint32_t j = 0;
                for (; j + 4 <= count; j += 4) {
                    const uint32x4x4_t d = vld4q_u32(reinterpret_cast<const uint32_t *>(vertices + size_t(j) * 16));
                    const int32x4_t d2 = vreinterpretq_s32_u32(d.val[2]);
                    float32x4x2_t st;
                    st.val[0] = vmulq_n_f32(vcvtq_f32_s32(vmulq_s32(vshrq_n_s32(d2, 16), vsc)), inv);
                    st.val[1] = vmulq_n_f32(vcvtq_f32_s32(vmulq_s32(vshrq_n_s32(vshlq_n_s32(d2, 16), 16), vtc)), inv);
                    vst2q_f32(tcOut + size_t(j) * 2, st);
                }

                scalar::computeTexcoords(vertices + size_t(j) * 16, count - j, sc, tc, tcOut + size_t(j) * 2);
            }

            inline uint32x4_t finiteMask(float32x4_t v) {
                return vcltq_f32(vabsq_f32(v), vdupq_n_f32(std::numeric_limits<float>::infinity()));
            }

            inline bool allLanes(uint32x4_t mask) {
                return vminvq_u32(mask) == 0xFFFFFFFFu;
            }

            bool canonicalizePositionBits(const float *src, uint32_t *dst, size_t count) {
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    const float32x4_t v = vld1q_f32(src + i);
                    if (!allLanes(finiteMask(v))) {
                        return false;
                    }

                    const uint32x4_t nonzero = vmvnq_u32(vceqzq_f32(v));
                    vst1q_u32(dst + i, vandq_u32(vreinterpretq_u32_f32(v), nonzero));
                }

                return scalar::canonicalizePositionBits(src + i, dst + i, count - i);
            }

            bool allFinite(const float *values, size_t count) {
                size_t i = 0;
                for (; i + 8 <= count; i += 8) {
                    const uint32x4_t a = finiteMask(vld1q_f32(values + i));
                    const uint32x4_t b = finiteMask(vld1q_f32(values + i + 4));
                    if (!allLanes(vandq_u32(a, b))) {
                        return false;
                    }
                }

                return scalar::allFinite(values + i, count - i);
            }

            bool indicesInRange(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
                if (vertexCount == 0) {
                    return count == 0;
                }

                const uint32_t lastOffset = (vertexCount > 0xFFFFFFFFull) ? 0xFFFFFFFFu : uint32_t(vertexCount - 1);
                const uint32x4_t vFirst = vdupq_n_u32(first);
                const uint32x4_t vLast = vdupq_n_u32(lastOffset);
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    const uint32x4_t index = vld1q_u32(indices + i);
                    const uint32x4_t ok = vandq_u32(vcgeq_u32(index, vFirst), vcleq_u32(vsubq_u32(index, vFirst), vLast));
                    if (!allLanes(ok)) {
                        return false;
                    }
                }

                return scalar::indicesInRange(indices + i, count - i, first, vertexCount);
            }

            bool floatsEqual(const float *a, const float *b, size_t count) {
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    if (!allLanes(vceqq_f32(vld1q_f32(a + i), vld1q_f32(b + i)))) {
                        return false;
                    }
                }

                return scalar::floatsEqual(a + i, b + i, count - i);
            }

            bool offsetIndicesEqual(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
                const uint32x4_t fa = vdupq_n_u32(firstA);
                const uint32x4_t fb = vdupq_n_u32(firstB);
                size_t i = 0;
                for (; i + 4 <= count; i += 4) {
                    const uint32x4_t da = vsubq_u32(vld1q_u32(a + i), fa);
                    const uint32x4_t db = vsubq_u32(vld1q_u32(b + i), fb);
                    if (!allLanes(vceqq_u32(da, db))) {
                        return false;
                    }
                }

                return scalar::offsetIndicesEqual(a + i, b + i, count - i, firstA, firstB);
            }

            void packBytesPresent(const uint8_t *seen, uint64_t *bits) {
                // No movemask on NEON: weight each nonzero byte by its bit and
                // horizontally add each 8-byte half into one mask byte.
                static const uint8_t weightsData[16] = { 1, 2, 4, 8, 16, 32, 64, 128, 1, 2, 4, 8, 16, 32, 64, 128 };
                const uint8x16_t weights = vld1q_u8(weightsData);
                for (uint32_t w = 0; w < 4; w++) {
                    uint64_t word = 0;
                    for (uint32_t k = 0; k < 4; k++) {
                        const uint8x16_t present = vtstq_u8(vld1q_u8(seen + w * 64 + k * 16), vld1q_u8(seen + w * 64 + k * 16));
                        const uint8x16_t weighted = vandq_u8(present, weights);
                        const uint64_t lo = vaddv_u8(vget_low_u8(weighted));
                        const uint64_t hi = vaddv_u8(vget_high_u8(weighted));
                        word |= (lo | (hi << 8)) << (k * 16);
                    }

                    bits[w] |= word;
                }
            }
        }
#endif

        // ---------------------------------------------------------------
        // Dispatch
        // ---------------------------------------------------------------

        static void dispatchConvertVertexAttributes(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX2) {
                x86::convertVertexAttributesAVX2(vertices, count, suppressFog, posOut, normColOut);
                return;
            }
            if (l >= Level::Vec128) {
                x86::convertVertexAttributes128(vertices, count, suppressFog, posOut, normColOut);
                return;
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                neon::convertVertexAttributes(vertices, count, suppressFog, posOut, normColOut);
                return;
            }
#endif
            scalar::convertVertexAttributes(vertices, count, suppressFog, posOut, normColOut);
        }

        static void dispatchTransformVertices(const float *pos, uint32_t count, const float *mvp, const float *scale, const float *translate, float *transformedOut, float *screenOut) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX) {
                x86::transformVerticesAVX(pos, count, mvp, scale, translate, transformedOut, screenOut);
                return;
            }
            if (l >= Level::Vec128) {
                x86::transformVertices128(pos, count, mvp, scale, translate, transformedOut, screenOut);
                return;
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                neon::transformVertices(pos, count, mvp, scale, translate, transformedOut, screenOut);
                return;
            }
#endif
            scalar::transformVertices(pos, count, mvp, scale, translate, transformedOut, screenOut);
        }

        static void dispatchComputeTexcoords(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX2) {
                x86::computeTexcoordsAVX2(vertices, count, sc, tc, tcOut);
                return;
            }
            if (l >= Level::Vec128) {
                x86::computeTexcoords128(vertices, count, sc, tc, tcOut);
                return;
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                neon::computeTexcoords(vertices, count, sc, tc, tcOut);
                return;
            }
#endif
            scalar::computeTexcoords(vertices, count, sc, tc, tcOut);
        }

        static bool dispatchCanonicalizePositionBits(const float *src, uint32_t *dst, size_t count) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX) {
                return x86::canonicalizePositionBitsAVX(src, dst, count);
            }
            if (l >= Level::Vec128) {
                return x86::canonicalizePositionBits128(src, dst, count);
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                return neon::canonicalizePositionBits(src, dst, count);
            }
#endif
            return scalar::canonicalizePositionBits(src, dst, count);
        }

        static bool dispatchAllFinite(const float *values, size_t count) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX) {
                return x86::allFiniteAVX(values, count);
            }
            if (l >= Level::Vec128) {
                return x86::allFinite128(values, count);
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                return neon::allFinite(values, count);
            }
#endif
            return scalar::allFinite(values, count);
        }

        static bool dispatchIndicesInRange(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX2) {
                return x86::indicesInRangeAVX2(indices, count, first, vertexCount);
            }
            if (l >= Level::Vec128) {
                return x86::indicesInRange128(indices, count, first, vertexCount);
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                return neon::indicesInRange(indices, count, first, vertexCount);
            }
#endif
            return scalar::indicesInRange(indices, count, first, vertexCount);
        }

        static bool dispatchFloatsEqual(const float *a, const float *b, size_t count) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX) {
                return x86::floatsEqualAVX(a, b, count);
            }
            if (l >= Level::Vec128) {
                return x86::floatsEqual128(a, b, count);
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                return neon::floatsEqual(a, b, count);
            }
#endif
            return scalar::floatsEqual(a, b, count);
        }

        static bool dispatchOffsetIndicesEqual(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX2) {
                return x86::offsetIndicesEqualAVX2(a, b, count, firstA, firstB);
            }
            if (l >= Level::Vec128) {
                return x86::offsetIndicesEqual128(a, b, count, firstA, firstB);
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                return neon::offsetIndicesEqual(a, b, count, firstA, firstB);
            }
#endif
            return scalar::offsetIndicesEqual(a, b, count, firstA, firstB);
        }

#if defined(RR64_SIMD_HAS_TMEM_SWIZZLE)
        namespace {
            // One eight-byte texel word: bytes reversed in each 32-bit half,
            // halves exchanged on odd rows (a full 64-bit byte reverse).
            inline void tmemTexelWordScalar(const uint8_t *src, uint8_t *dst, bool oddRowSwap) {
                uint32_t lo, hi;
                std::memcpy(&lo, src, 4);
                std::memcpy(&hi, src + 4, 4);
#   if defined(_MSC_VER) && !defined(__clang__)
                lo = _byteswap_ulong(lo);
                hi = _byteswap_ulong(hi);
#   else
                lo = __builtin_bswap32(lo);
                hi = __builtin_bswap32(hi);
#   endif
                std::memcpy(dst, oddRowSwap ? &hi : &lo, 4);
                std::memcpy(dst + 4, oddRowSwap ? &lo : &hi, 4);
            }

#   if defined(RR64_SIMD_X64)
            RR64_TARGET_AVX2 void tmemTexelWordsAVX2(const uint8_t *src, uint8_t *dst, uint32_t words, bool oddRowSwap) {
                const __m256i plain = _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                    3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
                const __m256i swapped = _mm256_setr_epi8(7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8,
                    7, 6, 5, 4, 3, 2, 1, 0, 15, 14, 13, 12, 11, 10, 9, 8);
                const __m256i order = oddRowSwap ? swapped : plain;
                uint32_t w = 0;
                for (; w + 4 <= words; w += 4) {
                    const __m256i bytes = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(src + size_t(w) * 8));
                    _mm256_storeu_si256(reinterpret_cast<__m256i *>(dst + size_t(w) * 8), _mm256_shuffle_epi8(bytes, order));
                }

                if (w + 2 <= words) {
                    tmemTexelWordPair(src + size_t(w) * 8, dst + size_t(w) * 8, oddRowSwap);
                    w += 2;
                }

                if (w < words) {
                    tmemTexelWordScalar(src + size_t(w) * 8, dst + size_t(w) * 8, oddRowSwap);
                }
            }
#   endif
        }

        void tmemTexelWords(const uint8_t *src, uint8_t *dst, uint32_t words, bool oddRowSwap) {
#   if defined(RR64_SIMD_X64)
            if (level() >= Level::AVX2) {
                tmemTexelWordsAVX2(src, dst, words, oddRowSwap);
                return;
            }
#   endif
            uint32_t w = 0;
            for (; w + 2 <= words; w += 2) {
                tmemTexelWordPair(src + size_t(w) * 8, dst + size_t(w) * 8, oddRowSwap);
            }

            if (w < words) {
                tmemTexelWordScalar(src + size_t(w) * 8, dst + size_t(w) * 8, oddRowSwap);
            }
        }
#endif

        void markBytesPresent(const uint8_t *data, size_t count, uint8_t *seen) {
            // Independent byte stores have no loop-carried dependency, unlike
            // `bits[b >> 6] |= ...`, which serializes whenever neighboring
            // palette indices share a 64-bit word (the common CI8 case).
            size_t i = 0;
            for (; i + 4 <= count; i += 4) {
                seen[data[i + 0]] = 1;
                seen[data[i + 1]] = 1;
                seen[data[i + 2]] = 1;
                seen[data[i + 3]] = 1;
            }

            for (; i < count; i++) {
                seen[data[i]] = 1;
            }
        }

        static void dispatchPackBytesPresent(const uint8_t *seen, uint64_t *bits) {
#if defined(RR64_SIMD_X64)
            const Level l = level();
            if (l >= Level::AVX2) {
                x86::packBytesPresentAVX2(seen, bits);
                return;
            }
            if (l >= Level::Vec128) {
                x86::packBytesPresent128(seen, bits);
                return;
            }
#elif defined(RR64_SIMD_ARM64)
            if (level() >= Level::Vec128) {
                neon::packBytesPresent(seen, bits);
                return;
            }
#endif
            scalar::packBytesPresent(seen, bits);
        }

        // ---------------------------------------------------------------
        // In-game verification (RR64_SIMD_VERIFY=1)
        // ---------------------------------------------------------------
        //
        // With RR64_SIMD_VERIFY=1 every vector kernel call is shadowed by the
        // scalar reference on the same live data and the results compared. The
        // first mismatches are printed to stderr with the kernel name; the
        // running count is available through verifyMismatches(). This turns a
        // normal play session into a correctness run on real game geometry.
        // It costs roughly a scalar pass per call, so it is opt-in only.

        namespace {
            bool verifyEnabled() {
                static const bool enabled = [] {
                    const char *setting = std::getenv("RR64_SIMD_VERIFY");
                    return (setting != nullptr) && (std::strcmp(setting, "1") == 0);
                }();
                return enabled;
            }

            std::atomic<uint64_t> verifyCalls{ 0 };
            std::atomic<uint64_t> verifyFailures{ 0 };

            bool sameFloat(float a, float b) {
                uint32_t x, y;
                std::memcpy(&x, &a, 4);
                std::memcpy(&y, &b, 4);
                return (x == y) || (std::isnan(a) && std::isnan(b));
            }

            void reportMismatch(const char *kernel, size_t count) {
                const uint64_t n = verifyFailures.fetch_add(1, std::memory_order_relaxed) + 1;
                if (n <= 16) {
                    std::fprintf(stderr, "[RR64-SIMD] VERIFY MISMATCH #%llu in %s (%s, count=%zu)\n",
                        static_cast<unsigned long long>(n), kernel, levelName(level()), count);
                }
            }

            // Shadow buffers reused per thread; packets are small.
            thread_local std::vector<float> shadowFloatsA;
            thread_local std::vector<float> shadowFloatsB;
            thread_local std::vector<uint8_t> shadowBytes;
            thread_local std::vector<uint32_t> shadowWords;
        }

        uint64_t verifyMismatches() {
            return verifyFailures.load(std::memory_order_relaxed);
        }

        uint64_t verifyCallCount() {
            return verifyCalls.load(std::memory_order_relaxed);
        }

        void convertVertexAttributes(const uint8_t *vertices, uint32_t count, bool suppressFog, float *posOut, uint8_t *normColOut) {
            dispatchConvertVertexAttributes(vertices, count, suppressFog, posOut, normColOut);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                shadowFloatsA.resize(size_t(count) * 3);
                shadowBytes.resize(size_t(count) * 4);
                scalar::convertVertexAttributes(vertices, count, suppressFog, shadowFloatsA.data(), shadowBytes.data());
                if ((std::memcmp(shadowFloatsA.data(), posOut, size_t(count) * 3 * sizeof(float)) != 0) ||
                    (std::memcmp(shadowBytes.data(), normColOut, size_t(count) * 4) != 0)) {
                    reportMismatch("convertVertexAttributes", count);
                }
            }
        }

        void transformVertices(const float *pos, uint32_t count, const float *mvp, const float *scale, const float *translate, float *transformedOut, float *screenOut) {
            dispatchTransformVertices(pos, count, mvp, scale, translate, transformedOut, screenOut);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                shadowFloatsA.resize(size_t(count) * 4);
                shadowFloatsB.resize(size_t(count) * 4);
                scalar::transformVertices(pos, count, mvp, scale, translate, shadowFloatsA.data(), shadowFloatsB.data());
                for (size_t k = 0; k < size_t(count) * 4; k++) {
                    if (!sameFloat(shadowFloatsA[k], transformedOut[k]) || !sameFloat(shadowFloatsB[k], screenOut[k])) {
                        reportMismatch("transformVertices", count);
                        break;
                    }
                }
            }
        }

        void computeTexcoords(const uint8_t *vertices, uint32_t count, int32_t sc, int32_t tc, float *tcOut) {
            dispatchComputeTexcoords(vertices, count, sc, tc, tcOut);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                shadowFloatsA.resize(size_t(count) * 2);
                scalar::computeTexcoords(vertices, count, sc, tc, shadowFloatsA.data());
                if (std::memcmp(shadowFloatsA.data(), tcOut, size_t(count) * 2 * sizeof(float)) != 0) {
                    reportMismatch("computeTexcoords", count);
                }
            }
        }

        bool canonicalizePositionBits(const float *src, uint32_t *dst, size_t count) {
            const bool result = dispatchCanonicalizePositionBits(src, dst, count);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                shadowWords.resize(count);
                const bool expected = scalar::canonicalizePositionBits(src, shadowWords.data(), count);
                if ((expected != result) || (result && (std::memcmp(shadowWords.data(), dst, count * sizeof(uint32_t)) != 0))) {
                    reportMismatch("canonicalizePositionBits", count);
                }
            }

            return result;
        }

        bool allFinite(const float *values, size_t count) {
            const bool result = dispatchAllFinite(values, count);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                if (result != scalar::allFinite(values, count)) {
                    reportMismatch("allFinite", count);
                }
            }

            return result;
        }

        bool indicesInRange(const uint32_t *indices, size_t count, uint32_t first, uint64_t vertexCount) {
            const bool result = dispatchIndicesInRange(indices, count, first, vertexCount);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                if (result != scalar::indicesInRange(indices, count, first, vertexCount)) {
                    reportMismatch("indicesInRange", count);
                }
            }

            return result;
        }

        bool floatsEqual(const float *a, const float *b, size_t count) {
            const bool result = dispatchFloatsEqual(a, b, count);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                if (result != scalar::floatsEqual(a, b, count)) {
                    reportMismatch("floatsEqual", count);
                }
            }

            return result;
        }

        bool offsetIndicesEqual(const uint32_t *a, const uint32_t *b, size_t count, uint32_t firstA, uint32_t firstB) {
            const bool result = dispatchOffsetIndicesEqual(a, b, count, firstA, firstB);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                if (result != scalar::offsetIndicesEqual(a, b, count, firstA, firstB)) {
                    reportMismatch("offsetIndicesEqual", count);
                }
            }

            return result;
        }

        void packBytesPresent(const uint8_t *seen, uint64_t *bits) {
            uint64_t expected[4] = { bits[0], bits[1], bits[2], bits[3] };
            dispatchPackBytesPresent(seen, bits);
            if (verifyEnabled() && (level() != Level::Scalar)) {
                verifyCalls.fetch_add(1, std::memory_order_relaxed);
                scalar::packBytesPresent(seen, expected);
                if (std::memcmp(expected, bits, sizeof(expected)) != 0) {
                    reportMismatch("packBytesPresent", 256);
                }
            }
        }
    };
};
