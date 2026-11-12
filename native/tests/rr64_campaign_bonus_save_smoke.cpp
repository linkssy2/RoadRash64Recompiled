#include "rr64_campaign_bonus_save.hpp"
#include "rr64_campaign_completion.hpp"
#include "rr64_engine_layout.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string_view>
#include <vector>

extern "C" void func_8001F960(unsigned char *, recomp_context *);
extern "C" void func_8001F9BC(unsigned char *, recomp_context *);
extern "C" void func_800207DC(unsigned char *, recomp_context *);
extern "C" void func_80020ECC(unsigned char *, recomp_context *);
extern "C" void func_8005F480(unsigned char *, recomp_context *);
extern "C" void func_8005F420(unsigned char *, recomp_context *);
extern "C" void test_bonus_native_save(unsigned char *, recomp_context *);
extern "C" void test_campaign_menu_entry(unsigned char *, recomp_context *);

namespace {
using namespace rr64::engine;
constexpr unsigned profile = 0x800D6A40u, cache = 0x800C07C8u,
                   buffer = 0x800C0470u;
constexpr unsigned stack = 0x807FE000u, status = 0x8009DE08u;
using File = std::array<unsigned char, 256>;
std::array<File, 6> files{};
std::array<bool, 6> exists{};
unsigned io_slot = 0, write_error = 0, read_error = 0, scan_error = 0;
unsigned writes = 0, reads = 0, checks = 0;
unsigned directory_reads = 0, deletes = 0;

void check(bool value, const char *message) {
  ++checks;
  if (!value) {
    std::fprintf(stderr, "Campaign bonus save: %s\n", message);
    std::exit(1);
  }
}
unsigned word(unsigned char *m, unsigned address) {
  unsigned result = 0;
  read_u32(m, address, result);
  return result;
}
std::vector<unsigned char> bytes(unsigned char *m, unsigned address,
                                 unsigned size) {
  std::vector<unsigned char> result(size);
  for (unsigned i = 0; i < size; ++i)
    read_u8(m, address + i, result[i]);
  return result;
}
void copy(unsigned char *m, unsigned destination, unsigned source,
          unsigned size) {
  const auto data = bytes(m, source, size);
  for (unsigned i = 0; i < size; ++i)
    write_s8(m, destination + i, data[i]);
}
void native_checksum(unsigned char *m, unsigned record) {
  write_u32(m, record + 4, 0);
  recomp_context ctx{};
  ctx.r4 = guest_address(record);
  func_8001F960(m, &ctx);
  write_u32(m, record + 4, unsigned(ctx.r2));
}
void seed(unsigned char *m, unsigned level, unsigned identity) {
  rr64_campaign_bonus_new_profile(m, profile);
  for (unsigned i = 0; i < 0xF8; i += 4)
    write_u32(m, profile + i, 0xA5000000u + identity * 0x100u + i);
  write_u32(m, profile, 4);
  write_u32(m, profile + 0x20, identity % 5);
  write_u32(m, profile + 0x3C, level);
  write_u32(m, profile + 0x50, 0x12345678u);
  native_checksum(m, profile);
}
unsigned bonus(unsigned char *m) {
  return rr64_campaign_qualification_word(m, profile + 0x54,
                                          word(m, profile + 0x54));
}
void save(unsigned char *m, unsigned slot) {
  io_slot = slot;
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  write_u32(m, stack + 0x74, profile);
  write_u32(m, buffer - 4, 0x11223344u);
  write_u32(m, buffer + 256, 0x55667788u);
  test_bonus_native_save(m, &ctx);
  check(unsigned(ctx.r16) == write_error, "native Pak write result retained");
  check(word(m, buffer - 4) == 0x11223344u &&
            word(m, buffer + 256) == 0x55667788u,
        "256-byte serialization preserves neighboring memory");
}
unsigned selected_load(unsigned char *m, unsigned slot) {
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  ctx.r4 = slot;
  ctx.r5 = guest_address(profile);
  func_80020ECC(m, &ctx);
  return unsigned(ctx.r2);
}
void invalidate(unsigned char *m) {
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  func_8001F9BC(m, &ctx);
  write_u32(m, 0x800C0450u, 0);
}
unsigned scan(unsigned char *m) {
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  func_800207DC(m, &ctx);
  return unsigned(ctx.r2);
}
unsigned load(unsigned char *m, unsigned slot) {
  // A real menu scan populates the native cache; selecting Load calls scan
  // again and must retain its validated bonus tail on the cache-hit return.
  invalidate(m);
  scan(m);
  return selected_load(m, slot);
}

void enter_menu(unsigned char *m, unsigned mode) {
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  ctx.r16 = mode;
  ctx.r18 = ctx.r3 = guest_address(0x800A0000u);
  ctx.r2 = guest_address(0x800A4FF0u);
  test_campaign_menu_entry(m, &ctx);
  check(word(m, 0x800A1810u) == mode, "native dispatcher commits requested mode before scanning saves");
  check(!rr64_campaign_passive_scan(), "passive scan scope ends before the native menu initializes");
}

// Reach Thrash from fresh memory without ever selecting a Big Game profile.
// Expected masks come from the native unlock helper, including reversed tiers.
void verify_startup_unlocks() {
  constexpr unsigned unlocks = 0x800A5334u;
  unsigned startup_cases = 0;
  for (unsigned scenario = 0; scenario < 14; ++scenario) {
    std::vector<unsigned char> boot(kRdramSize);
    auto *m = boot.data();
    files = {};
    exists = {};
    read_error = scan_error = write_error = 0;
    unsigned expected_level = 0, expected_special = 0;
    if (scenario != 7) {
      const unsigned chapter = scenario < 5 ? scenario : scenario == 6 || scenario == 11 ? 5 : scenario == 10 ? 6 : 4;
      seed(m, chapter, scenario == 5 ? 3 : 0);
      if (scenario < 5 || scenario == 11)
        write_u32(m, profile + 0x50, 0); // Progress need not finish this chapter.
      native_checksum(m, profile);
      save(m, 0);
      if (scenario == 9 || scenario == 11) {
        // The real writer fixes the version and downgrades incomplete bonuses;
        // construct these unsupported values only in the simulated device file.
        write_u32(m, buffer + (scenario == 9 ? 0 : 0x3C), scenario == 9 ? 3 : 5);
        native_checksum(m, buffer);
        for (unsigned i = 0; i < files[0].size(); ++i)
          read_u8(m, buffer + i, files[0][i]);
      }
      if (scenario == 8)
        files[0][0x38] ^= 1; // Valid-looking chapter, invalid native checksum.
      if (scenario < 5) expected_level = scenario;
      if (scenario == 5) { expected_level = 4; expected_special = 1u << 5; }
      if (scenario == 6) { expected_level = 4; expected_special = (1u << 6) | (1u << 7); }
    }
    std::fill(boot.begin(), boot.end(), 0);
    recomp_context ctx{};
    ctx.r4 = guest_address(profile);
    func_8005F480(m, &ctx);
    ctx.r4 = 0; ctx.r5 = 1;
    func_8005F420(m, &ctx);
    const auto live = bytes(m, profile, 0xF8);
    const auto saved_files = files;
    const unsigned old_reads = reads, old_writes = writes, old_deletes = deletes;
    const unsigned old_directories = directory_reads;
    if (scenario == 12) read_error = 9;
    if (scenario == 13) scan_error = 8;
    // Boot/controller init, racing and Thrash itself must not perform Pak I/O.
    for (unsigned mode : {1u, 3u, 33u}) enter_menu(m, mode);
    check(reads == old_reads && directory_reads == old_directories,
          "non-main modes do not scan saves");
    enter_menu(m, 32);

    auto expected = boot;
    for (unsigned tier = 0; tier < 15; ++tier)
      write_u16(expected.data(), unlocks + tier * 2, 0);
    ctx = {};
    ctx.r4 = expected_level; ctx.r5 = 1;
    func_8005F420(expected.data(), &ctx);
    for (unsigned tier = 5; tier <= 7; ++tier) {
      if (!(expected_special & (1u << tier))) continue;
      ctx.r4 = tier; ctx.r5 = 0;
      func_8005F420(expected.data(), &ctx);
    }
    if (bytes(m, unlocks, 30) != bytes(expected.data(), unlocks, 30)) {
      std::fprintf(stderr, "Startup case %u: saved chapter=%u status=%u; actual/expected unlocked tiers:",
                   scenario, word(m, cache + 0x3C), word(m, status));
      for (unsigned tier = 0; tier < 15; ++tier) {
        std::uint16_t actual = 0, wanted = 0;
        read_u16(m, unlocks + tier * 2, actual);
        read_u16(expected.data(), unlocks + tier * 2, wanted);
        std::fprintf(stderr, " %u:%u/%u", tier, actual, wanted);
      }
      std::fputc('\n', stderr);
    }
    check(bytes(m, unlocks, 30) == bytes(expected.data(), unlocks, 30),
          "cold scan restores only earned normal and special tiers before selecting a save");
    check(bytes(m, profile, 0xF8) == live && writes == old_writes && files == saved_files,
          "startup unlock scan preserves live profile, cash, bike and every saved payload");
    check(deletes == old_deletes,
          "passive startup scan never deletes valid, corrupt or unsupported save slots");
    check(word(m, 0x800A77C8u) == 0 && word(m, 0x800A77CCu) == 0,
          "earned unlock restoration does not enable an unlock-all cheat");
    if (scenario < 8 || scenario == 10 || scenario == 11) {
      check(directory_reads == old_directories + 1 && reads == old_reads + (scenario == 7 ? 0 : 1),
            "fresh startup scans the directory and reads only present slots");
      for (unsigned entry = 0; entry < 3; ++entry) enter_menu(m, 32);
      check(directory_reads == old_directories + 1 && reads == old_reads + (scenario == 7 ? 0 : 1),
            "repeated menu entry reuses the native save cache");
    }
    ++startup_cases;
  }
  read_error = scan_error = 0;

  // Force a real uncached scan through the helper and preserve every register
  // the native dispatcher needs for its indirect menu initializer call.
  std::vector<unsigned char> memory(kRdramSize);
  auto *m = memory.data();
  files = {}; exists = {};
  write_u32(m, 0x800A1810u, 32);
  recomp_context ctx{};
  ctx.r29 = guest_address(stack);
  ctx.r4 = guest_address(0x800256C0u);
  ctx.r2 = ctx.r18 = guest_address(0x800A0000u);
  ctx.r16 = guest_address(0x800A5070u);
  ctx.r17 = guest_address(0x800A1BDCu);
  ctx.r31 = 0x8004850Cu;
  ctx.f0.fl = 1.0f; ctx.f12.fl = 4.25f;
  const auto original_context = ctx;
  const unsigned old_directories = directory_reads;
  rr64_campaign_menu_unlocks(m, &ctx);
  check(directory_reads == old_directories + 1 &&
            std::memcmp(&ctx, &original_context, sizeof(ctx)) == 0,
        "uncached startup scan preserves caller registers and floating-point state");
  check(!rr64_campaign_passive_scan(), "passive flag is not retained after scanning");
  std::printf("Startup unlock scan: %u fresh profiles and saved/error cases passed.\n", startup_cases);
}
} // namespace

// Model the Pak device and its name encoding, not the game scan. The complete
// native scan owns cache hits, invalidation, directory traversal, status flags,
// checksums, profile copies and error reporting.
extern "C" void func_8000C3AC(unsigned char *m, recomp_context *ctx) {
  ++writes;
  check(unsigned(ctx->r7) == 256, "native write requests exactly256bytes");
  const unsigned source = word(m, unsigned(ctx->r29) + 0x10);
  check(source == buffer, "native write uses its original staging buffer");
  if (write_error == 0) {
    for (unsigned i = 0; i < 256; ++i)
      read_u8(m, source + i, files[io_slot][i]);
    exists[io_slot] = true;
  }
  ctx->r2 = write_error;
}
extern "C" void func_8000C2AC(unsigned char *m, recomp_context *ctx) {
  ++reads;
  check(unsigned(ctx->r7) == 256, "native read requests exactly256bytes");
  const unsigned destination = word(m, unsigned(ctx->r29) + 0x10);
  check(destination == buffer, "native read uses its original staging buffer");
  std::uint8_t slot = 255;
  read_u8(m, unsigned(ctx->r6), slot);
  check(slot < files.size(), "native read identifies a valid Pak extension");
  if (read_error == 0 && exists[slot])
    for (unsigned i = 0; i < 256; ++i)
      write_s8(m, destination + i, files[slot][i]);
  ctx->r2 = read_error != 0 ? read_error : exists[slot] ? 0 : 1;
}
extern "C" void func_8000B8BC(unsigned char *m, recomp_context *) {
  write_u16(m, 0x800B0AA0u, 1);
}
extern "C" void func_8000C124(unsigned char *m, recomp_context *ctx) {
  write_u32(m, unsigned(ctx->r5), files.size());
  unsigned used = 0;
  for (bool present : exists)
    used += present;
  write_u32(m, unsigned(ctx->r6), used);
  ctx->r2 = scan_error;
}
extern "C" void func_8000C224(unsigned char *m, recomp_context *ctx) {
  write_u32(m, unsigned(ctx->r5), 0x1000);
  ctx->r2 = 0;
}
extern "C" void func_8000C5D0(unsigned char *m, recomp_context *ctx) {
  ++directory_reads;
  check(unsigned(ctx->r7) == files.size(), "native scan uses Pak directory capacity");
  for (unsigned slot = 0; slot < files.size(); ++slot) {
    const unsigned record = unsigned(ctx->r5) + slot * 32;
    write_u32(m, unsigned(ctx->r6) + slot * 4, exists[slot] ? 0 : 5);
    write_u32(m, record, 256);
    write_u32(m, record + 4, 0x4E524F45u);
    write_u16(m, record + 8, 0x3738u);
    write_s8(m, record + 10, slot);
  }
  ctx->r2 = 0;
}
extern "C" void func_8000C050(unsigned char *m, recomp_context *ctx) {
  std::uint8_t slot = 255;
  read_u8(m, unsigned(ctx->r4), slot);
  check(slot < files.size() && unsigned(ctx->r6) == 1,
        "native scan decodes one slot-letter extension");
  write_s8(m, unsigned(ctx->r5), 'A' + slot);
}
extern "C" void func_8000C518(unsigned char *m, recomp_context *ctx) {
  ++deletes;
  std::uint8_t slot = 255;
  read_u8(m, word(m, unsigned(ctx->r29) + 0x10), slot);
  check(slot < files.size(), "native delete identifies a valid Pak extension");
  exists[slot] = false;
  ctx->r2 = 0;
}

// Persist only the simulated Pak payloads and independently captured live
// profile expectations. A separate process reads this fixture into fresh RDRAM;
// this does not substitute for the runtime virtual-Pak .mpk implementation.
void write_fixture(const char *path,
                   const std::array<std::vector<unsigned char>, 6> &profiles) {
  std::ofstream out(path, std::ios::binary);
  out.write("RR64BS01", 8);
  for (const auto &file : files)
    out.write(reinterpret_cast<const char *>(file.data()), file.size());
  for (const auto &record : profiles)
    out.write(reinterpret_cast<const char *>(record.data()), record.size());
  out.close();
  check(out.good(), "separate-process Pak fixture saved successfully");
}
void read_fixture(const char *path,
                  std::array<std::vector<unsigned char>, 6> &profiles) {
  std::ifstream in(path, std::ios::binary);
  std::array<char, 8> magic{};
  in.read(magic.data(), magic.size());
  check(std::string_view(magic.data(), magic.size()) == "RR64BS01",
        "separate-process Pak fixture has the expected version");
  for (auto &file : files)
    in.read(reinterpret_cast<char *>(file.data()), file.size());
  for (auto &record : profiles) {
    record.resize(0xF8);
    in.read(reinterpret_cast<char *>(record.data()), record.size());
  }
  check(in.good() && in.peek() == std::char_traits<char>::eof(),
        "separate-process Pak fixture is complete and has no trailing bytes");
  exists.fill(true);
}
void verify_cached_session(
    unsigned char *m,
    const std::array<std::vector<unsigned char>, 6> &initial_profiles) {
  const unsigned initial_reads = reads, initial_directories = directory_reads;
  check(scan(m) == 0, "fresh-session native directory scan succeeds");
  check(reads == initial_reads + 6 && directory_reads == initial_directories + 1,
        "fresh-session scan reads all six saved files exactly once");
  std::uint16_t cached = 0;
  read_u16(m, 0x800C07C0u, cached);
  check(cached == 1, "native scan sets its cache-valid flag");
  for (unsigned menu_frame = 0; menu_frame < 3; ++menu_frame) {
    check(scan(m) == 0, "repeated native menu scan succeeds from cache");
    for (unsigned slot = 0; slot < 6; ++slot)
      check(rr64_campaign_bonus_record(m, cache + slot * 0xF8) == 1 &&
                word(m, cache + slot * 0xF8 + 0x3C) == 4,
            "cached menu scan retains bonus identity and compatible base record");
  }
  for (unsigned slot = 0; slot < 6; ++slot) {
    check(selected_load(m, slot) == 1 && word(m, profile + 0x3C) == 5 &&
              bonus(m) == 0xFEDCBA98u - slot * 0x11111111u,
          "cold-start cached selection restores each independent bonus result");
    check(bytes(m, profile, 0xF8) == initial_profiles[slot],
          "cold-start cached selection preserves every cash, bike, inventory and reputation byte");
  }
  recomp_context reset_profile{};
  reset_profile.r4 = guest_address(profile);
  func_8005F480(m, &reset_profile);
  check(selected_load(m, 3) == 1 &&
            bytes(m, profile, 0xF8) == initial_profiles[3] &&
            bonus(m) == 0xFEDCBA98u - 3 * 0x11111111u,
        "new campaign initialization does not invalidate native cached saved profiles");
  check(reads == initial_reads + 6 && directory_reads == initial_directories + 1,
        "cached menu frames and selected loads perform no Pak reads");

  const File before_replacement = files[2];
  for (unsigned i = 0xF8; i < 256; ++i)
    files[2][i] = 0;
  check(scan(m) == 0 && rr64_campaign_bonus_record(m, cache + 2 * 0xF8) == 1,
        "native cache remains authoritative until explicitly invalidated");
  invalidate(m);
  check(scan(m) == 0 && rr64_campaign_bonus_record(m, cache + 2 * 0xF8) == 0,
        "actual rescan rejects a replaced file's absent bonus extension");
  check(selected_load(m, 2) == 1 && word(m, profile + 0x3C) == 4,
        "replaced ordinary save cannot reuse previously cached bonus results");
  const auto completed_profile = bytes(m, profile, 0xF8);
  rr64_campaign_results_begin(m);
  check(rr64_campaign_repeat_completion(m),
        "fresh-process loaded native completion prevents a repeated ending");
  check(bytes(m, profile, 0xF8) == completed_profile,
        "fresh-process completion decision preserves spent cash and bike");
  files[2] = before_replacement;
  exists[5] = false;
  invalidate(m);
  check(scan(m) == 0 && rr64_campaign_bonus_record(m, cache + 5 * 0xF8) == 0,
        "actual rescan clears extension metadata for a removed slot");
  exists[5] = true;
  invalidate(m);
  check(scan(m) == 0 && rr64_campaign_bonus_record(m, cache + 5 * 0xF8) == 1,
        "actual rescan captures a restored slot independently");

}

int main(int argc, char **argv) {
  std::vector<unsigned char> memory(kRdramSize);
  auto *m = memory.data();
  std::array<std::vector<unsigned char>, 6> initial_profiles;
  const bool writer = argc == 3 && std::string_view(argv[1]) == "--write-fixture";
  const bool reader = argc == 3 && std::string_view(argv[1]) == "--read-fixture";
  check(argc == 1 || writer || reader,
        "usage: RR64CampaignBonusSaveSmoke [--write-fixture|--read-fixture path]");
  if (reader) {
    read_fixture(argv[2], initial_profiles);
    verify_cached_session(m, initial_profiles);
    check(writes == 0 && deletes == 0,
          "fresh-process reload never rewrites or removes simulated Pak saves");
    std::printf("Campaign bonus save fresh-process read: %u checks passed; "
                "%u native reads, %u writes\n", checks, reads, writes);
    return 0;
  }

  // Full nibble range survives native I/O, including signed high-bit words.
  for (unsigned slot = 0; slot < 6; ++slot) {
    seed(m, 5, slot + 1);
    const unsigned results = 0xFEDCBA98u - slot * 0x11111111u;
    const unsigned reputation = word(m, profile + 0x54);
    check(rr64_campaign_qualification_store(m, profile + 0x54, results) ==
              reputation,
          "bonus writes preserve original reputation word");
    const auto live = bytes(m, profile, 0xF8);
    initial_profiles[slot] = live;
    save(m, slot);
    check(bytes(m, profile, 0xF8) == live,
          "native save preserves live profile and index5");
    check(word(m, buffer + 0x3C) == 4,
          "serialized profile keeps backward-compatible index4");
    for (unsigned i = 0; i < 0xF8; ++i) {
      if ((i >= 4 && i < 8) || (i >= 0x3C && i < 0x40))
        continue;
      check(files[slot][i] == live[i],
            "every original result, inventory and statistic survives");
    }
    // An original build copies only248bytes and still receives the exact
    // completed original campaign. No extra profile word is overwritten.
    copy(m, 0x80300000u, buffer, 0xF8);
    check(word(m, 0x8030003Cu) == 4 && word(m, 0x80300050u) == 0x12345678u &&
              word(m, 0x80300054u) == reputation,
          "old-style profile copy remains valid completed main campaign");
    seed(m, 0, 99);
    check(load(m, slot) == 1, "native selected load succeeds");
    check(word(m, profile + 0x3C) == 5 && bonus(m) == results,
          "selected slot restores its full independent bonus word");
    check(word(m, profile + 0x54) == reputation, "load preserves reputation");
  }

  if (writer) {
    write_fixture(argv[2], initial_profiles);
    std::printf("Campaign bonus save fixture written: six 256-byte simulated "
                "Pak files; %u checks passed\n", checks);
    return 0;
  }
  std::vector<unsigned char> restarted(kRdramSize);
  m = restarted.data();
  verify_cached_session(m, initial_profiles);

  // Each track has all16 native counter values, including zeros and caps.
  for (unsigned track = 0; track < 8; ++track)
    for (unsigned count = 0; count < 16; ++count) {
      seed(m, 5, 7);
      const unsigned results = count << (track * 4);
      rr64_campaign_qualification_store(m, profile + 0x54, results);
      save(m, 0);
      seed(m, 0, 98);
      check(
          load(m, 0) == 1 && word(m, profile + 0x3C) == 5 &&
              bonus(m) == results,
          "all eight full4bit counters roundtrip through actual native copies");
    }

  // Base saves are accepted byte-for-byte; merely reading never adds a tail.
  seed(m, 4, 2);
  const auto original = bytes(m, profile, 0xF8);
  save(m, 0);
  const File legacy = files[0];
  check(word(m, buffer + 0xF8) == 0 && word(m, buffer + 0xFC) == 0,
        "ordinary save retains native zero padding");
  seed(m, 0, 98);
  const unsigned saved_writes = writes;
  check(load(m, 0) == 1 && bytes(m, profile, 0xF8) == original,
        "old completed save loads without profile conversion");
  check(writes == saved_writes && files[0] == legacy,
        "load never writes or upgrades save automatically");
  rr64_campaign_results_begin(m);
  check(rr64_campaign_repeat_completion(m),
        "legacy completed save suppresses replay ending after native save/load");
  check(bytes(m, profile, 0xF8) == original && files[0] == legacy,
        "completion decision neither spends money nor rewrites older saves");

  seed(m, 5, 4);
  rr64_campaign_qualification_store(m, profile + 0x54, 0x10203040u);
  save(m, 0);
  const File valid = files[0];
  // Corrupt every bit of the extension; preserve valid base campaign and
  // reject bonus activation, including corruption in the integrity word.
  for (unsigned bit = 0; bit < 64; ++bit) {
    files[0] = valid;
    files[0][0xF8 + bit / 8] ^= 1u << (bit % 8);
    seed(m, 0, 98);
    check(load(m, 0) == 1 && word(m, profile + 0x3C) == 4,
          "corrupt extension falls back to completed original campaign");
    check(rr64_campaign_bonus_record(m, cache) == 0,
          "invalid tail never claims bonus unlock");
  }
  files[0] = valid;
  for (unsigned i = 0xF8; i < 256; ++i)
    files[0][i] = 0;
  seed(m, 0, 98);
  check(load(m, 0) == 1 && word(m, profile + 0x3C) == 4,
        "truncated or old-stylezero tail keeps main progress intact");

  // A valid base checksum is not enough to attach another profile's tail.
  // Change cash, recompute the original checksum, and retain the old tail.
  for (unsigned i = 0; i < 256; ++i)
    write_s8(m, buffer + i, valid[i]);
  write_u32(m, buffer + 0x38, word(m, buffer + 0x38) + 123);
  native_checksum(m, buffer);
  for (unsigned i = 0; i < 256; ++i)
    read_u8(m, buffer + i, files[0][i]);
  seed(m, 0, 98);
  check(load(m, 0) == 1 && word(m, profile + 0x3C) == 4,
        "native-checksummed changed profile cannot borrow another bonus tail");

  files[0] = valid;
  seed(m, 0, 98);
  check(load(m, 0) == 1 && bonus(m) == 0x10203040u, "valid cache primed");
  check(rr64_campaign_bonus_record(m, cache) == 1,
        "validated slot exposes bonus unlock");
  write_u32(m, cache + 0x38, word(m, cache + 0x38) + 1);
  check(rr64_campaign_bonus_record(m, cache) == 0,
        "slot cache binds to exact profile contents");
  rr64_campaign_bonus_scan(m);
  check(rr64_campaign_bonus_record(m, cache) == 0,
        "new Pak scan clears extension cache");
  check(rr64_campaign_bonus_record(m, cache + 1) == 0 &&
            rr64_campaign_bonus_record(m, cache + 6 * 0xF8) == 0,
        "unaligned and out-of-range slots rejected");

  // Failed writes do not alter the existing file; failed loads do not erase
  // the current unsaved bonus progress or reuse a previous slot extension.
  seed(m, 5, 5);
  rr64_campaign_qualification_store(m, profile + 0x54, 0x76543210u);
  const auto unsaved = bytes(m, profile, 0xF8);
  files[0] = valid;
  write_error = 7;
  save(m, 0);
  check(files[0] == valid && bytes(m, profile, 0xF8) == unsaved &&
            bonus(m) == 0x76543210u,
        "failed native write preserves disk and live profile");
  write_error = 0;
  scan_error = 8;
  // Native directory failures clear slot availability and set the Pak error
  // word; they do not return the fabricated scan error used by the old stub.
  check(load(m, 0) == ~0u && word(m, 0x800C0450u) == 8 &&
            bytes(m, profile, 0xF8) == unsaved &&
            bonus(m) == 0x76543210u,
        "failed scan leaves current unsaved bonus unchanged");
  check(rr64_campaign_bonus_record(m, cache) == 0,
        "failed scan cannot reuse cached tail");
  scan_error = 0;
  read_error = 9;
  check(load(m, 0) == 9 && bytes(m, profile, 0xF8) == unsaved &&
            bonus(m) == 0x76543210u,
        "failed native read leaves current unsaved bonus unchanged");
  read_error = 0;
  check(deletes == 0, "all valid native base profiles survived complete Pak scans");

  // The original profile initializer invokes the reset and clears counters;
  // changing RDRAM mappings also cannot borrow another session's progress.
  recomp_context fresh{};
  fresh.r4 = guest_address(profile);
  func_8005F480(m, &fresh);
  write_u32(m, profile + 0x3C, 5);
  check(bonus(m) == 0, "native New Game clears bonus progress");
  rr64_campaign_qualification_store(m, profile + 0x54, 0xFFFFFFFFu);
  std::vector<unsigned char> other(kRdramSize);
  write_u32(other.data(), profile + 0x3C, 5);
  check(bonus(other.data()) == 0,
        "new memory mapping has no stale bonus progress");
  check(rr64_campaign_qualification_word(nullptr, profile + 0x54, 123) == 123,
        "null mapping remains a native passthrough");
  for (unsigned level = 0; level < 5; ++level) {
    seed(m, level, level);
    const unsigned address = profile + 0x40 + level * 4;
    check(rr64_campaign_qualification_word(m, address, 0xA5A55A5Au) ==
                  0xA5A55A5Au &&
              rr64_campaign_qualification_store(m, address, 0x01020304u) ==
                  0x01020304u,
          "all original campaign qualification loads and stores pass through");
  }

  verify_startup_unlocks();

  std::printf("Campaign bonus save: %u checks passed; %u native writes, %u "
              "native reads\n",
              checks, writes, reads);
}
