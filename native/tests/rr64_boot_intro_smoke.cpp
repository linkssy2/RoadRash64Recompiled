#include "recomp.h"
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <atomic>
extern unsigned image;
extern std::atomic<bool> player_started_title_session;
extern "C" void rr64_intro_skip(unsigned char*);
void func_80021B0C(uint8_t*, recomp_context*);
void func_80071F94(uint8_t*, recomp_context*);
void check(bool ok) { if (!ok) { std::fputs("Boot intro check failed\n", stderr); std::exit(1); } }
int main() {
    std::vector<uint8_t> memory(8 * 1024 * 1024);
    auto* rdram = memory.data();
    // A button held during boot must be released before it can skip anything.
    MEM_W(0, (int32_t)0x800A1830) = 200;
    MEM_H(0, (int32_t)0x8009CD90) = 0x8000;
    rr64_intro_skip(rdram);
    check(MEM_W(0, (int32_t)0x800A1830) == 200);
    for (unsigned frame : {20u, 199u, 200u, 299u, 300u, 399u, 400u, 499u, 500u, 599u, 600u, 800u}) {
        for (unsigned button : {0u, 0x4000u, 0x8000u, 0x1000u, 0x9000u}) {
            recomp_context context{}; auto* ctx = &context; ctx->r29 = (int32_t)0x807FF000;
            MEM_H(0, (int32_t)0x8009CD90) = 0;
            rr64_intro_skip(rdram);
            player_started_title_session.store(false);
            MEM_W(0, (int32_t)0x800A1830) = frame;
            MEM_W(0, (int32_t)0x800B0808) = 320;
            MEM_W(0, (int32_t)0x800B080C) = 240;
            MEM_W(0, (int32_t)0x8009E2C0) = 0;
            MEM_H(0, (int32_t)0x8009CD90) = button;
            MEM_H(0, (int32_t)0x8009CDA8) = 0;
            MEM_W(0, (int32_t)0x800A1814) = 1;
            MEM_W(0, (int32_t)0x8009CC50) = 99;
            image = 0;
            func_80021B0C(rdram, ctx);
            const bool skip = frame >= 200 && frame < 600 && (button & 0x9000);
            const unsigned expected = skip ? (frame / 100 + 1) * 100 : frame;
            check((unsigned)MEM_W(0, (int32_t)0x800A1830) == expected);
            if (expected >= 600) check(image == 9);
            if (frame < 200) check(image == 10);
            const bool title_accept = frame >= 600 && (button & 0x9000);
            check(player_started_title_session.load() == title_accept);
            check(MEM_W(0, (int32_t)0x800A1814) == (title_accept ? 32 : 1));
            check(MEM_W(0, (int32_t)0x8009CC50) == (title_accept ? 0 : 99));
            // Holding through the newly selected page must not advance again.
            rr64_intro_skip(rdram);
            check((unsigned)MEM_W(0, (int32_t)0x800A1830) == expected);
        }
    }
    for (unsigned button : {0u, 0x4000u, 0x8000u, 0x1000u}) {
        recomp_context context{}; auto* ctx = &context; ctx->r29 = (int32_t)0x807FF000;
        MEM_W(0, (int32_t)0x8009E2C0) = 1;
        MEM_H(0, (int32_t)0x8009F250) = 0;
        MEM_B(0, (int32_t)0x800A98A0) = 1;
        MEM_H(0, (int32_t)0x8009CDA8) = button;
        MEM_W(0, (int32_t)0x800A1814) = 1;
        func_80071F94(rdram, ctx);
        check(MEM_W(0, (int32_t)0x800A1814) == ((button & 0x1000) ? 32 : 1));
    }
    std::puts("Boot intro: logo boundaries, one page per press, held input, A/Start session activation and native title transition passed");
}
