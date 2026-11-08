// RR64-SIMD: refuse to start on an x86-64 CPU below the build's instruction
// floor, with a readable message instead of an illegal-instruction crash.
//
// Every other translation unit is compiled for the floor (AVX, or AVX2 with
// RR64_X86_BASELINE=AVX2), and some of them run AVX code from static
// initializers (hlslpp's vector constants), i.e. before main(). This file is
// compiled at the generic x86-64 level (native/cmake/Simd.cmake strips the
// floor for it) and runs ahead of those initializers:
//   Windows  #pragma init_seg(lib): .CRT$XCL, before the user segment .CRT$XCU.
//   ELF      constructor priority 101: .init_array.00101 sorts before the
//            default-priority initializers of the executable.
//   macOS    no initializer priorities on Mach-O, but the floor is moot there:
//            every Intel Mac supported by macOS 11 (the deployment target) has
//            AVX, and the arm64 build has no x86 floor at all.
//
// Keep this file free of C++ templates and inline library functions. A
// template instantiated here could be merged at link time with a copy from an
// AVX-compiled translation unit, and the merged copy might be the AVX one.

#if defined(_M_X64) || defined(__x86_64__)

#if defined(_WIN32)
#   define WIN32_LEAN_AND_MEAN
#   include <windows.h>
#   include <intrin.h>
#else
#   include <cpuid.h>
#   include <stdio.h>
#   include <unistd.h>
#endif

namespace {
    void cpuidQuery(unsigned leaf, unsigned subleaf, unsigned regs[4]) {
#if defined(_WIN32)
        int r[4];
        __cpuidex(r, int(leaf), int(subleaf));
        regs[0] = unsigned(r[0]);
        regs[1] = unsigned(r[1]);
        regs[2] = unsigned(r[2]);
        regs[3] = unsigned(r[3]);
#else
        __cpuid_count(leaf, subleaf, regs[0], regs[1], regs[2], regs[3]);
#endif
    }

    unsigned long long xcr0() {
#if defined(_MSC_VER) && !defined(__clang__)
        return _xgetbv(0);
#else
        unsigned eax = 0, edx = 0;
        __asm__ volatile("xgetbv" : "=a"(eax), "=d"(edx) : "c"(0));
        return (static_cast<unsigned long long>(edx) << 32) | eax;
#endif
    }

    bool meetsFloor() {
        unsigned r[4] = {};
        cpuidQuery(0, 0, r);
        const unsigned maxLeaf = r[0];
        cpuidQuery(1, 0, r);
        const unsigned ecx1 = r[2];
        const bool sse42 = (ecx1 & (1u << 20)) != 0;
        const bool popcnt = (ecx1 & (1u << 23)) != 0;
        const bool osxsave = (ecx1 & (1u << 27)) != 0;
        const bool avx = (ecx1 & (1u << 28)) != 0;
        // The OS must preserve XMM and YMM state, or AVX instructions fault.
        if (!sse42 || !popcnt || !osxsave || !avx || ((xcr0() & 0x6) != 0x6)) {
            return false;
        }

#if defined(RR64_BASELINE_AVX2)
        // x86-64-v3: AVX2, FMA, BMI1, BMI2, F16C, LZCNT, MOVBE.
        const bool fma = (ecx1 & (1u << 12)) != 0;
        const bool movbe = (ecx1 & (1u << 22)) != 0;
        const bool f16c = (ecx1 & (1u << 29)) != 0;
        if ((maxLeaf < 7) || !fma || !movbe || !f16c) {
            return false;
        }

        cpuidQuery(7, 0, r);
        const bool bmi1 = (r[1] & (1u << 3)) != 0;
        const bool avx2 = (r[1] & (1u << 5)) != 0;
        const bool bmi2 = (r[1] & (1u << 8)) != 0;
        cpuidQuery(0x80000001u, 0, r);
        const bool lzcnt = (r[2] & (1u << 5)) != 0;
        return bmi1 && avx2 && bmi2 && lzcnt;
#else
        (void)maxLeaf;
        return true;
#endif
    }

#if defined(RR64_BASELINE_AVX2)
#   define RR64_FLOOR_NAME "AVX2 (Intel Haswell / AMD Excavator or newer)"
#else
#   define RR64_FLOOR_NAME "AVX (Intel Sandy Bridge / AMD Bulldozer or newer)"
#endif

    void refuseToStart() {
        static const char message[] =
            "Road Rash 64 Recompiled needs a processor with " RR64_FLOOR_NAME
            " instructions, and an operating system that enables them.\n\n"
            "This computer's processor does not report them, so the game cannot start.";
#if defined(_WIN32)
        MessageBoxA(nullptr, message, "Road Rash 64 Recompiled", MB_OK | MB_ICONERROR);
        ExitProcess(1);
#else
        fputs(message, stderr);
        fputs("\n", stderr);
        _exit(1);
#endif
    }

    void checkFloor() {
        if (!meetsFloor()) {
            refuseToStart();
        }
    }
}

#if defined(_MSC_VER)
#   pragma warning(disable : 4073) // initializers put in library initialization area
#   pragma init_seg(lib)
namespace {
    struct FloorCheck {
        FloorCheck() { checkFloor(); }
    };

    FloorCheck floorCheck;
}
#elif defined(__APPLE__)
__attribute__((constructor)) static void rr64CheckFloor() {
    checkFloor();
}
#else
__attribute__((constructor(101))) static void rr64CheckFloor() {
    checkFloor();
}
#endif

#endif
