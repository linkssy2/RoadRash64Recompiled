# RR64-SIMD correctness harness and microbenchmark. ROM-free and independent
# of the game build; EXCLUDE_FROM_ALL so ordinary builds never compile it.
#
#   cmake --build <build> --target RR64SimdHarness
#   <build>/bin/RR64SimdHarness            bit-exactness vs. the scalar loops
#   <build>/bin/RR64SimdHarness --bench    plus per-level timings
add_executable(RR64SimdHarness EXCLUDE_FROM_ALL
    tests/simd/rr64_simd_harness.cpp
    "${RT64_ROOT}/src/common/rt64_rr64_simd.cpp")
target_include_directories(RR64SimdHarness PRIVATE
    "${RT64_ROOT}/src"
    "${RT64_ROOT}/src/contrib/hlslpp/include")
target_compile_features(RR64SimdHarness PRIVATE cxx_std_20)
