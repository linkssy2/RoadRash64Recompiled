# SIMD pass: AVX floor, AVX2 dispatch

This document records the instruction-set floor, the hand-vectorized kernels
and how they are validated. Every change site in the source carries an
`RR64-SIMD:` comment with its rationale.

**Compatibility note.** The default x86-64 floor moves from Nehalem (SSE4.2) to
AVX: Sandy Bridge / Ivy Bridge / Bulldozer (2011) and newer. On an older CPU
the game now shows a message and exits (see *Startup check*) instead of
running. AVX2 code paths are chosen at run time, so one build serves both AVX
and AVX2 CPUs.

## Instruction-set floor

| Target | Floor | Wider paths |
| --- | --- | --- |
| Windows x64, Linux x86-64 | AVX (`-march=x86-64-v2 -mavx`, `/arch:AVX`) | AVX2 kernels chosen at run time |
| arm64 (not built by this project today) | `-mcpu=apple-m1` on Apple, ARMv8-A elsewhere | NEON |

`-DRR64_X86_BASELINE=AVX2` raises the x86 floor to x86-64-v3 for a personal
build. The floor is set once in `native/cmake/Simd.cmake` and applies to every
target (game, N64ModernRuntime, RT64, RecompFrontend, Opus, importer helpers).

Why it must be global: hlslpp, RT64 and RecompFrontend share inline headers,
and hlslpp changes code paths per ISA. Previously RT64 compiled at plain
x86-64 while the game target used `-march=nehalem`; that mismatch was benign
at SSE4 but becomes an ODR and layout break as soon as AVX enters the picture.

### Floating-point contraction is off everywhere

`-ffp-contract=off` (clang/GCC, `/clang:` for clang-cl; `/fp:precise` for
cl.exe) is now global. On x86 the AVX floor has no FMA anyway, so the game
simulation is bit-identical to the stock Nehalem builds; this setting keeps an
AVX2-floor build (which has FMA) identical too, which online races need. It
also covers any future arm64 build, where compilers contract `a*b+c` into FMA
by default. (The MK64 importer helpers
already used `-ffp-contract=off` / `/fp:strict` for the same reason.)

### hlslpp stays in 128-bit mode

`HLSLPP_NO_SIMD_REGISTER_256` (new patch:
`dependency-patches/native_lib_rt64_src_contrib_hlslpp.patch`) keeps hlslpp's
128-bit layouts when the compiler targets AVX. Without it, `-mavx` would

- change `float4x4` from 16- to 32-byte alignment in every RT64 consumer,
- change the summation order of `mul(float4, float4x4)`, so every transformed
  vertex's low bits would differ from today's builds, and
- fail to compile with GCC (anonymous aggregates with copy-assignable members).

The 128-bit hlslpp code is still VEX-encoded and uses AVX permutes. With this
setting, RT64's math on the AVX floor is bit-identical to the stock build.

### Startup check

`native/src/rr64_cpu_check.cpp` is compiled without AVX and runs before every
other static initializer (hlslpp's vector constants already execute AVX
instructions before `main`). On a CPU or OS without AVX (or the AVX2 set for an
AVX2-floor build) the game shows a message box and exits instead of crashing
with an illegal instruction. Verified under qemu: a Nehalem CPU gets the
message, a Sandy Bridge CPU runs the AVX build, and an AVX2-floor build refuses
Sandy Bridge and runs on Haswell. The file avoids C++ templates on purpose: a
template instantiated there could be merged at link time with an AVX copy.

## Kernels

All kernels live in `native/lib/rt64/src/common/rt64_rr64_simd.{h,cpp}`
(installed from `dependency-overrides/`). Each has a scalar reference that is
the original loop; vector paths are bit-identical to it.

| Kernel | Replaces | Paths |
| --- | --- | --- |
| `convertVertexAttributes` | per-vertex int16 position / color byte loop in `RSP::setVertexCommon` | SSE4.1 (also the AVX path), AVX2, NEON; record-wise since pass 2 |
| `transformVertices` | hlslpp clip-space transform, divide by `float3(w, -w, w)`, viewport map | SSE4.1, AVX (2 vertices per register), NEON (LD3/ST4 SoA) |
| `computeTexcoords` | `(float)((double)(s * sc) / 2^21)` per vertex | SSE4.1, AVX2, NEON |
| `canonicalizePositionBits` | finite check + signed-zero canonicalization before geometry hashing | SSE4.1, AVX, NEON |
| `allFinite`, `indicesInRange` | `RR64GeometryMatch::validGeometry` scans | SSE4.1, AVX/AVX2, NEON |
| `floatsEqual`, `offsetIndicesEqual` | `compatibleGeometry` / `compatibleTopology` scans | SSE4.1, AVX/AVX2, NEON |
| `markBytesPresent` + `packBytesPresent` | TMEM hasher v5 CI8 palette bitset | store-only marking, movemask / NEON pack |
| `tmemPaletteWordPair`, `tmemTexelWordPair` | RR64's SSE2-only TMEM two-word load paths | one PSHUFB (x86), TBL/REV (arm64) |
| `tmemTexelWords` | whole contiguous TMEM row segments in `loadToTMEMCommon` (pass 2) | 32-byte VPSHUFB (AVX2), PSHUFB / REV pairs |

Notes on exactness:

- **Transform.** The kernels execute hlslpp's own per-lane operation sequence
  (multiply, the `madd` steps, divide by `float3(w, -w, w)`, scale, translate),
  so both 16-byte records, including the screen record's padding lane, match
  hlslpp bit for bit. NEON uses `vmlaq_f32` exactly like hlslpp, so the result
  matches under any contraction setting. The only exception is a NaN produced
  by a degenerate matrix: it is NaN on every path, but its sign and payload are
  compiler-defined even for hlslpp (LLVM may commute operands or fold `x / -w`
  into `-(x / w)`).
- **Texcoords.** `(float)((double)p / 2^21) == (float)p * 2^-21` for every int32
  `p`: the double holds `p` exactly, the power-of-two scale is exact in both
  widths, and both sides round `p` to float once. The harness checks every
  int16 `s`/`t` against 48 scale values.
- **Viewport padding lane.** `interop::float3` is a 12-byte struct; the old
  expression converted it to `hlslpp::float3` implicitly and that conversion
  defines the padding lane. The call site converts the same way before passing
  16-byte storage to the kernel.
- **Floor-independent.** The transform kernel follows `HLSLPP_SIMD_REGISTER_256`
  and `__FMA__`, so it stays exact if hlslpp's configuration ever changes.

### Evaluated and left alone

- **XXH3 runtime dispatch (AVX2).** The hashed volumes per frame (TMEM rows,
  geometry positions) are small enough that the gain is a few microseconds;
  XXH3's output is ISA-independent either way. AVX2-floor builds and arm64
  already get the AVX2/NEON XXH3 inline.
- **RSP vector unit (audio microcode).** Already SSE4.1 on x86; an RSP vector
  is exactly 128 bits, so wider registers do not help. The AVX floor
  VEX-encodes it.
- **Audio mixers** (music, course audio, voice). A few hundred stereo frames per
  buffer, and the resamplers accumulate a double cursor serially, so a vector
  version could not stay bit-exact.
- **Recompiled MIPS.** Scalar by nature; it benefits from the floor flags only.

## Validation

`RR64SimdHarness` (`native/tests/simd/rr64_simd_harness.cpp`, CMake target of
the same name) runs every kernel at every level the CPU supports and compares
against the scalar reference: random and edge-case inputs (signed zeros,
denormals, infinities, NaNs, degenerate `w = 0` projections, index range
boundaries, every packet size 0 to 70 for tail handling, sentinel-guarded
buffers to catch overruns), exhaustive int16 texcoords, the TMEM swizzles
against both the scalar `loadWord` mapping and the SSE2 sequences they
replace, and (pass 2) the TMEM run kernel at every length plus 20,000 random
whole LoadTile/LoadBlock loads through a model of `loadToTMEMCommon` (TMEM
wraps, DXT bank swaps, odd word counts, unaligned sources) compared byte for
byte against the one-word path.

| Configuration | Result |
| --- | --- |
| clang 18, AVX floor (x86-64-v2 + AVX), levels scalar / SSE4.1 / AVX / AVX2 | pass, 295,244,284 element comparisons, 0 mismatches |
| same binary on an emulated Sandy Bridge (AVX, no AVX2) and Haswell | pass, 205,736,573 / 0 and 295,244,284 / 0 |
| clang 18, AVX2 floor (x86-64-v3, FMA) | pass, 295,244,284 / 0 |
| GCC 13, AVX floor and AVX2 floor | pass, 295,244,284 / 0 (each) |
| clang 18, arm64 (`-mcpu=apple-m1`), contraction off and on, under qemu | pass, 106,628,862 / 0 (each) |

RR64's own geometry-match and TMEM-hash smokes give the same digests at every
level with `RR64_SIMD_VERIFY=1`.

`RR64_SIMD_VERIFY=1` repeats the comparison during real play: every kernel
call is shadowed by the scalar reference on live game data, mismatches are
printed to stderr, and the startup log line `[RR64-SIMD]` reports the active
level. `RR64_SIMD=scalar|vec128|avx|avx2` caps the level for A/B testing.

### Measured (x86, Xeon 2.1 GHz VM with AVX2)

ns per element (vertex, float, index or TMEM byte), best of several runs;
pass 1 -> pass 2 where pass 2 changed the kernel.

| Kernel | scalar | SSE4.1 | AVX | AVX2 |
| --- | --- | --- | --- | --- |
| vertex attributes, 32/packet | 1.89 | 0.81 -> 0.69 | 0.81 -> 0.69 | 0.78 -> 0.57 (3.3x) |
| vertex transform, 32/packet | 1.88 | 1.64 | 1.24 | 1.23 (1.5x) |
| texcoords, 32/packet | 1.04 | 0.52 | 0.52 | 0.46 (2.3x) |
| whole `setVertexCommon` packet body (sum) | 4.81 | 2.85 | 2.45 | 2.26 (2.1x) |
| canonicalize position bits | 1.28 | 0.19 | 0.145 -> 0.114 | 0.147 -> 0.114 (11.2x) |
| allFinite | 0.57 | 0.14 | 0.08 | 0.08 (7.1x) |
| floatsEqual | 0.71 | 0.18 | 0.131 -> 0.128 | 0.131 -> 0.128 (5.5x) |
| indicesInRange | 0.72 | 0.247 -> 0.165 | 0.247 -> 0.165 | 0.142 -> 0.116 (6.2x) |
| offsetIndicesEqual | 0.42 | 0.18 -> 0.136 | 0.18 -> 0.136 | 0.160 -> 0.137 (3.1x) |
| CI8 palette presence (original loop 2.48) | 0.30 | 0.22 | 0.22 | 0.22 (11x vs original) |
| TMEM LoadBlock 4 KB (per byte) | | pairs 0.150 -> runs 0.076 | same | 0.150 -> 0.075 (2.0x) |
| TMEM LoadTile 4 KB, 32 x 16 words | | pairs 0.068 -> runs 0.060 | same | 0.068 -> 0.060 (1.13x) |

The SSE4.1 column matters beyond the vec128 cap: on an AVX-only CPU (Sandy /
Ivy Bridge) the integer kernels run their 128-bit
form, because 256-bit integer compares need AVX2. Pass 2's unrolled 128-bit
index scans are aimed at exactly those machines.

### Tuning pass 2

- **Vertex attributes, record-wise.** One byte shuffle per 16-byte record
  places x, y, z in the high halves of four dwords; a shift and a convert give
  the floats, and each vertex is written with one 16-byte store that overlaps
  the next vertex's x (which that vertex's store then overwrites). The final
  vertex of a packet stores exactly 12 bytes, so nothing is written past the
  stream. Colors use four shuffles that drop each record's reversed color into
  its own dword, ORed into one store (AVX2: plus one cross-lane permute). This
  removes the 4x4/8x4 transposes and the xyz blend network: +35% on AVX2, +17%
  on SSE4.1/AVX.
- **Scans, 16 elements per branch.** `canonicalizePositionBits`, `floatsEqual`,
  `indicesInRange` and `offsetIndicesEqual` test two vectors per branch (the
  128-bit forms two xmm). `offsetIndicesEqual` also compares `a - b` against
  `firstA - firstB`, one subtract per pair instead of two (identical in
  wrapping arithmetic).
- **TMEM rows.** `loadToTMEMCommon` (non-TLUT, non-RGBA32) now copies the
  longest run of words whose first `run - 1` steps cannot wrap TMEM or swap
  the DXT bank in one `tmemTexelWords` call and applies those steps
  arithmetically; the step after the run still goes through the original
  `loadWordStep`, so wraps and bank transitions are handled exactly as before.
  LoadBlock with typical DXT values runs 16 words at a time: 2x.
- **Tried and dropped.** The transform kernel with four vertices per iteration
  (two independent divide chains): no change, it is bound by divider
  throughput, and the divide must stay a divide for bit-exactness. Texcoords
  in the record-wise form: 10-15% slower than the transpose form (the integer
  multiply already amortizes the transpose), so the pass-1 kernel stays.

The transform kernel was first written SoA (8 vertices per AVX register);
measured against hlslpp at realistic packet sizes it was no faster, because
RT64 wants 16-byte AoS records back and the transposes ate the gain. The
shipped AoS form (two vertices per AVX register, padding lane exact) is the
one above. The palette scan's gain is algorithmic (stores instead of a
read-modify-write chain), so it holds at every level including scalar.

These are kernel timings on a shared virtual machine, best of several runs;
treat them as ratios, not absolute numbers. In-game gains are smaller than
the kernel ratios because the kernels are a part of each frame's CPU work.
The NEON paths were validated under arm64 emulation only, not timed.

## Building

Nothing changes in the build steps. `RR64_X86_BASELINE` (`AVX`, the default,
or `AVX2`) is the only new CMake option:

```
cmake -S native -B native/build ... -DRR64_X86_BASELINE=AVX2   # personal AVX2-floor build
cmake --build native/build --target RR64SimdHarness
native/build/bin/RR64SimdHarness            # bit-exactness; add --bench for timings
```

At run time `RR64_SIMD=scalar|vec128|avx|avx2` caps the kernel level and
`RR64_SIMD_VERIFY=1` shadows every kernel call with the scalar reference; the
`[RR64-SIMD]` line in the diagnostics log shows the build floor and the
active level.
