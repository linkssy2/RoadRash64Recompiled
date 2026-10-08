#pragma once

// Included after the smoke fixture's memory helpers. These are real native
// allocations, physical voices and filters; no game thread or sound device runs.
extern "C" {
void func_8007FE9C(unsigned char *, recomp_context *);
void func_80084084(unsigned char *, recomp_context *);
void func_800824E8(unsigned char *, recomp_context *);
void __OsSchedInstall(unsigned char *, recomp_context *);
void osCartRomInit_recomp(unsigned char *, recomp_context *c) { c->r2 = int(0x804f0000); }
void osAiSetFrequency_recomp(unsigned char *, recomp_context *c) { c->r2 = c->r4; }
void osCreateThread_recomp(unsigned char *, recomp_context *) {}
void osStartThread_recomp(unsigned char *, recomp_context *) {}
void func_80088EC0(unsigned char *, recomp_context *) {} // DMA message queue
void osSetIntMask_recomp(unsigned char *, recomp_context *c) { c->r2 = 0; }
void osScAddClient(unsigned char *, recomp_context *) {}
void MusPtrBankInitialize(unsigned char *, recomp_context *) {} // ROM-bank relocation
void func_80080BDC(unsigned char *, recomp_context *) {} // effect-bank relocation
using RivalNativeInitFunction = void (*)(unsigned char *, recomp_context *);
RivalNativeInitFunction rr64_rival_engine_fixture_lookup(unsigned address) {
    check(address == 0x80084084, "native init uses expected DMA callback factory");
    return func_80084084;
}
}

void test_rival_engine_init_cases() {
    constexpr unsigned config = 0x80500000, heap = 0x80600000;
    constexpr unsigned heap_extra = 0x14000, extra_voices = 14, canary = 0x619ad307;
    for (unsigned effects : {8u, 16u}) for (unsigned television : {0u, 1u}) {
        memory = initial; m = memory.data();
        // The loaded ROM supplies reverb/filter coefficients, but these are
        // native BSS pointers, not initialized constants from that ROM range.
        put(0x800b8470, 0); put(0x800b8474, 0); put(0x800df750, 0);
        const unsigned original_heap = effects == 8 ? 0x17c00 : 0x1e828;
        auto allocation = context(); allocation.r3 = original_heap; allocation.r5 = original_heap;
        const auto before = allocation;
        rr64_rival_engine_heap(m, &allocation);
        const unsigned bytes = unsigned(allocation.r5);
        check(bytes == original_heap + heap_extra && allocation.r3 == allocation.r5,
              "audio heap hook grows both allocator and stored capacity");
        allocation.r3 = before.r3; allocation.r5 = before.r5;
        check(std::memcmp(&allocation, &before, sizeof(before)) == 0,
              "heap hook preserves unrelated registers");
        for (unsigned i = 0; i < 0x44; i += 4) put(config + i, 0);
        put(config + 4, effects); put(config + 12, 90);
        put(config + 16, heap); put(config + 20, bytes);
        put(config + 36, 64); put(config + 40, 256); put(config + 44, 44100);
        put(config + 48, 4096); put(config + 52, 1);
        put(config + 56, effects == 8 ? 18 : 32);
        put(config + 60, effects == 8 ? 512 : 1024);
        put(0x80000300, television);
        for (unsigned i = 0; i < 16; i += 4) {
            put(heap - 16 + i, canary); put(heap + bytes + i, canary);
        }
        call(func_8007FE9C, config);
        // The audio thread's first callback allocates scheduler queues. Execute
        // that allocator without starting its infinite device-processing loop.
        call(__OsSchedInstall, 0);
        const unsigned expanded = effects + extra_voices;
        const unsigned used = word(0x800dfa04) - word(0x800dfa00);
        check(word(config + 4) == expanded && word(0x800df6e4) == expanded + 4,
              "real native init sizes the complete logical pool");
        check(rr64_rival_engine_effect_limit(expanded + 4) == effects + 4,
              "original effect rows remain separate after native init");
        check(used > 0 && used <= bytes, "native filters and voice buffers fit expanded heap");
        check(word(config + 48) == 8192 &&
                  word(config + 56) == (effects == 8 ? 18u : 32u) + 2 * extra_voices,
              "native init expands command storage and DMA cache with voices");
        const unsigned command = word(0x800df998), voices = word(0x800df6e8);
        check(command >= heap && command + 8192 * 8 <= heap + used,
              "native command buffer has full expanded capacity inside heap");
        for (unsigned i = 0; i < expanded; ++i) {
            const unsigned voice = voices + i * 0x1c, physical = word(voice + 8);
            check(physical >= heap && physical + 0x8c <= heap + used && word(physical + 8) == voice,
                  "each logical effect owns a physical voice and matching backpointer");
            for (unsigned j = 0; j < i; ++j)
                check(physical != word(voices + j * 0x1c + 8), "physical voices are distinct");
        }
        for (unsigned i = 0; i < 16; i += 4)
            check(word(heap - 16 + i) == canary && word(heap + bytes + i) == canary,
                  "native init preserves both audio heap boundary canaries");
    }
    memory = initial; m = memory.data();
    for (unsigned capacity : {0u, 0x10000u, 0x17c01u}) {
        auto c = context(); c.r3 = c.r5 = capacity; const auto saved = c;
        rr64_rival_engine_heap(m, &c);
        check(std::memcmp(&c, &saved, sizeof(c)) == 0, "unknown heap configuration stays native");
    }
    for (unsigned count : {0u, 7u, 9u, 15u, 17u}) {
        put(config + 4, count); put(config + 20, 0x17c00 + heap_extra);
        const std::vector<unsigned char> before(m + (config & 0x7fffff), m + (config & 0x7fffff) + 0x44);
        auto c = context(config); const auto saved = c;
        rr64_rival_engine_audio_init(m, &c);
        check(std::memcmp(&c, &saved, sizeof(c)) == 0 &&
                  std::memcmp(before.data(), m + (config & 0x7fffff), before.size()) == 0,
              "unknown voice configuration stays untouched");
    }
    for (unsigned count : {8u, 16u}) {
        put(config + 4, count); put(config + 20, count == 8 ? 0x17c00 : 0x1e828);
        auto c = context(config);
        rr64_rival_engine_audio_init(m, &c);
        check(word(config + 4) == count,
              "native-sized heap cannot accidentally receive expanded voice allocations");
    }
    for (unsigned address : {0u, config + 1, 0x807ffffcu}) {
        auto c = context(address); const auto saved = c;
        rr64_rival_engine_audio_init(m, &c);
        check(std::memcmp(&c, &saved, sizeof(c)) == 0, "invalid init pointer preserves caller registers");
    }
    rr64_rival_engine_audio_reset(); m = nullptr;
}
