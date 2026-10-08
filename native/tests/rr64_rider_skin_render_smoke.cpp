#include "rr64_engine_layout.hpp"
#include <algorithm>
#include "rr64_mk64_item_state.hpp"
#include "rr64_rider_skin_render.hpp"
#include "rr64_rider_skins.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>

extern "C" void fixture_material_call_rider(unsigned char *, recomp_context *);
extern "C" unsigned fixture_material_resolve(unsigned, const unsigned *, bool);
using namespace rr64;
using mk64_items::MaterialCommand;
namespace {
unsigned checks = 0, allocations = 0, cursor = 0x1200000, selected = 1,
         preview = 1;
bool highlights = false;
unsigned highlight_calls = 0;
mk64_items::RiderState effect{};
constexpr unsigned node = 0x80210000, entity = 0x80220000, dl = 0x80300000;
constexpr unsigned main_base = 0x80400000, call = main_base + 0x148,
                   model_table = 0x80310000;
void check(bool b, const char *s) {
  ++checks;
  if (!b) {
    std::fprintf(stderr, "FAIL %s (%u)\n", s, checks);
    std::exit(1);
  }
}
unsigned word(unsigned char *m, unsigned a) {
  unsigned n;
  std::memcpy(&n, m + (a & 0x7fffffff), 4);
  return n;
}
void put(unsigned char *m, unsigned a, unsigned v) {
  std::memcpy(m + (a & 0x7fffffff), &v, 4);
}
void command(unsigned char *m, unsigned a, MaterialCommand c) {
  put(m, a, c.first);
  put(m, a + 4, c.second);
}
void header(unsigned char *m, unsigned donor, unsigned slot, unsigned address) {
  const unsigned skin = donor == 10 ? 33 : 0, w = slot == 3 ? 32 : 64,
                 h = slot == 3 ? 16 : 32;
  put(m, 0x800a1578 + donor * 8, skin);
  put(m, 0x800d3840 + (skin * 4 + slot) * 8, address);
  put(m, address, 0x16);
  engine::write_u16(m, address + 0x22, 512);
  engine::write_u16(m, address + 0x26, w * h);
  put(m, address + 0x2c, w);
  put(m, address + 0x30, h);
  put(m, address + 0x34, 8);
}
void configure(unsigned char *m, unsigned epoch) {
  put(m, 0x8009cba4, epoch & 1);
  put(m, 0x800a1830, epoch);
  put(m, 0x800ac658 + (epoch & 1) * 4, main_base);
  put(m, 0x8009cb90, main_base);
  put(m, 0x800bc9a0, 0x4650);
  put(m, 0x800ac650, call);
  command(m, call, {0xde000000, dl});
}
std::vector<MaterialCommand> list(unsigned char *m, unsigned p) {
  std::vector<MaterialCommand> out;
  for (unsigned i = 0; i < 4200; ++i) {
    MaterialCommand c{word(m, p + i * 8), word(m, p + i * 8 + 4)};
    out.push_back(c);
    if (c.first == 0xdf000000)
      return out;
  }
  check(false, "bounded terminated list");
  return {};
}
std::array<MaterialCommand, 12> source{{{0xda380002, 0x05000000},
                                        {0xfc129bff, 0xfffdf638},
                                        {0xe200001c, 0xc8112230},
                                        {0xfd500000, 0x07000000},
                                        {0xfd100000, 0x07000800},
                                        {0xf5481000, 0},
                                        {0x0100600c, 0x05000100},
                                        {0x06000204, 0x0006080a},
                                        {0xfd500000, 0x08000000},
                                        {0xfd100000, 0x08000800},
                                        {0x06000204, 0x0006080a},
                                        {0xdf000000, 0}}};
} // namespace
namespace recomp {
void *alloc(unsigned char *m, std::size_t size) {
  ++allocations;
  const auto p = cursor;
  cursor += (unsigned(size) + 15) & ~15u;
  check(cursor < 64 * 1024 * 1024, "bounded extended arena");
  return m + p;
}
void free(unsigned char *, void *) {}
} // namespace recomp
namespace rr64::mk64_items {
bool render_effect(unsigned char *, unsigned, RiderState &out,
                   unsigned &clock) noexcept {
  out = effect;
  clock = 100;
  return true;
}
} // namespace rr64::mk64_items
extern "C" unsigned rr64_rider_skin_actor_selection(unsigned char *,
                                                    unsigned n) {
  return n == node ? selected : 0;
}
extern "C" unsigned rr64_rider_skin_preview_selection(unsigned char *,
                                                      unsigned n) {
  return n == node ? preview : 0;
}
extern "C" int rr64_highlights_presenting() { return highlights; }
extern "C" void rr64_highlights_weapon_draw(unsigned char *m,
                                            recomp_context *ctx, unsigned n) {
  ++highlight_calls;
  check(n == node && word(m, 0x800ac650) == unsigned(ctx->r5),
        "native highlight continuation cursor");
}
int main(int argc, char **argv) {
  std::array<rider_skins::Appearance, 5> skins;
  for (unsigned i = 0; i < skins.size(); ++i) {
    auto &a = skins[i];
    a.id = i == 4 ? "asymmetric" : i == 3 ? "shell" : i == 2 ? "other" : i ? "female" : "male";
    a.name = i == 4 ? "Asymmetric" : i == 3 ? "Shell" : i == 2 ? "Other" : i ? "Female" : "Male";
    a.donor = i == 1 ? 10 : 0;
    a.turtle_shell = i == 3;
    a.dual_head = i == 4;
    for (unsigned j = 0; j < 4; ++j) {
      auto &t = a.textures[j];
      t.width = j == 3 ? 32 : 64;
      t.height = j == 3 ? 16 : 32;
      t.bytes.assign(t.width * t.height + 512,
                     static_cast<unsigned char>(17 + i * 32 + j));
      if (j == 3)
        for (unsigned y = 0; y < t.height; ++y)
          for (unsigned x = 0; x < t.width; ++x)
            t.bytes[y * t.width + x] =
                static_cast<unsigned char>(20 + i * 32 + x + y * 5);
      for (unsigned k = t.width * t.height; k < t.bytes.size(); ++k)
        t.bytes[k] = 0xff;
    }
  }
  check(rider_skins::install_catalog(skins), "synthetic opaque skin catalog");
  rider_skins::begin_session();
  std::vector<unsigned char> memory(64 * 1024 * 1024);
  auto *m = memory.data();
  put(m, node, 2);
  put(m, node + 4, entity);
  put(m, model_table, dl);
  put(m, entity + 0x5bc, 0);
  for (unsigned donor : {0u, 10u})
    for (unsigned slot = 0; slot < 4; ++slot)
      header(m, donor, slot, 0x80500000 + donor * 0x4000 + slot * 0x1000);
  for (unsigned lod = 0; lod < 3; ++lod) {
    engine::write_u16(m, node + 0x4a + lod * 2, 2);
    put(m, node + 0x7c + lod * 32, 0x80500000);
    put(m, node + 0x80 + lod * 32, 0x80501000);
  }
  for (unsigned i = 0; i < source.size(); ++i)
    command(m, dl + i * 8, source[i]);
  std::array<MaterialCommand, 20> pure{};
  const rider_skins::ImageBinding b{0x07000000, 0x07000800, 0x82000000,
                                    0x82000800};
  check(rider_skins::replace_images(source, pure, std::span(&b, 1)) == 2,
        "only matching image and palette rewritten");
  for (unsigned i = 0; i < source.size(); ++i)
    if (i != 3 && i != 4)
      check(pure[i] == source[i], "native geometry and materials preserved");
  for (unsigned op : {0xde, 0xdf, 0xdd, 0x04, 0x64, 0xe0}) {
    auto bad = source;
    bad[6].first = op << 24;
    check(!rider_skins::replace_images(bad, pure, std::span(&b, 1)),
          "nested or extended source refused");
  }
  unsigned scenarios = 0;
  for (unsigned gender = 0; gender < 2; ++gender) {
    selected = gender + 1;
    for (unsigned lod = 0; lod < 3; ++lod) {
      // Explicit rendered LOD differs from stale simulation LOD, as in
      // highlights.
      engine::write_u16(m, node + 0x44, (lod + 1) % 3);
      put(m, node + 0x7c + lod * 32, 0x80500000 + (gender ? 10 : 0) * 0x4000);
      put(m, node + 0x80 + lod * 32, 0x80501000 + (gender ? 10 : 0) * 0x4000);
      for (unsigned kind = 0; kind < 4; ++kind) {
        effect = {};
        if (kind == 1)
          effect.star_until = 200;
        if (kind == 2)
          effect.boo_until = 200;
        if (kind == 3)
          effect.shrink_until = 400;
        configure(m, 1 + scenarios);
        recomp_context ctx{};
        ctx.r19 = int32_t(node);
        ctx.r23 = int32_t(0x800b0000);
        ctx.r30 = int32_t(model_table);
        ctx.r21 = lod;
        ctx.r22 = 0;
        highlights = (kind & 1) != 0;
        fixture_material_call_rider(m, &ctx);
        check(word(m, call) == 0xe0525464 && word(m, call + 8) == 0x6400002c &&
                  word(m, call + 16) == 0xde000000,
              "exact native caller enables extended addresses before clone "
              "branch");
        check(word(m, 0x800ac650) == call + 24,
              "exact native caller advances complete scoped branch");
        const unsigned cloned = word(m, call + 20);
        check(cloned >= 0x81000000, "clone lies beyond original address masks");
        const auto out = list(m, cloned);
        unsigned image_count = 0, palette_count = 0, geometry = 0;
        for (const auto &c : out) {
          if (c.first == 0xfd500000) {
            ++image_count;
            check((c.second & 0x80000000) != 0, "full texture address");
            const auto offset = fixture_material_resolve(
                c.second, std::array<unsigned, 16>{}.data(), true);
            const auto value = m[offset ^ 3];
            check(value == 17 + gender * 32 + (image_count - 1),
                  "authored texture byte guest order");
          }
          if (c.first == 0xfd100000) {
            ++palette_count;
            check(m[(c.second & 0x7fffffff) ^ 3] == 0xff,
                  "authored palette guest order");
          }
          if (c.first == 0xda380002 || c.first == 0x0100600c ||
              c.first == 0x06000204)
            ++geometry;
        }
        check(image_count == 2 && palette_count == 2 && geometry == 4,
              "all native rider draw geometry retained");
        check(out[out.size() - 3] == MaterialCommand{0x6400002c, 0} &&
                  out[out.size() - 2] ==
                      MaterialCommand{0xe0525464, 0x20000000},
              "native addressing restored");
        for (unsigned i = 0; i < source.size(); ++i)
          check(word(m, dl + i * 8) == source[i].first &&
                    word(m, dl + i * 8 + 4) == source[i].second,
                "shared donor list untouched");
        ++scenarios;
      }
    }
  }
  check(allocations == 3,
        "one immutable texture bank and two recycled frame buffers");
  check(highlight_calls == 12,
        "all six donor/LOD combinations use native highlight continuation");
  highlights = false;
  effect = {};
  selected = 1;
  preview = 1;
  // Menu graph emits direct pointers. The renderer captures that one graph,
  // preserving its geometry and invalidating only the native texture cache.
  configure(m, 30);
  put(m, 0x8009db2c, 0x1234);
  rr64_rider_skin_preview_begin(m, node);
  check(word(m, 0x8009db2c) == 0,
        "preview forces first texture load for same-donor neighbors");
  auto direct = source;
  direct[3].second = 0x80500040;
  direct[4].second = 0x80500840;
  direct[8].second = 0x00501040;
  direct[9].second = 0x00501840;
  for (unsigned i = 0; i + 1 < direct.size(); ++i)
    command(m, call + i * 8, direct[i]);
  put(m, 0x800ac650, call + (unsigned(direct.size()) - 1) * 8);
  rr64_rider_skin_preview_end(m);
  check(word(m, 0x800ac650) == call + 24 && word(m, call + 16) == 0xde000000,
        "preview redirects only freshly emitted graph commands");
  const auto preview_list = list(m, word(m, call + 20));
  unsigned count = 0;
  for (const auto &c : preview_list)
    if (c.first == 0xfd500000) {
      ++count;
      check(c.second >= 0x81000000, "preview pixels use authored bank");
    }
  check(count == 2,
        "both cached and physical native preview addresses resolve");
  check(allocations == 3, "preview reuses frame and texture bank");
  const unsigned first_preview = word(m, call + 20), next_call = call + 24;
  preview = 3;
  rr64_rider_skin_preview_begin(m, node);
  check(word(m, 0x8009db2c) == 0,
        "same-donor neighbor still emits a complete texture load");
  for (unsigned i = 0; i + 1 < direct.size(); ++i)
    command(m, next_call + i * 8, direct[i]);
  put(m, 0x800ac650, next_call + (unsigned(direct.size()) - 1) * 8);
  rr64_rider_skin_preview_end(m);
  const auto next_preview = list(m, word(m, next_call + 20));
  check(next_preview[3].second != preview_list[3].second &&
            m[(next_preview[3].second & 0x7fffffff) ^ 3] == 81,
        "same native donor can display two distinct local skin textures");
  check(list(m, first_preview) == preview_list,
        "later local preview preserves earlier frame-owned commands");
  preview = 0;
  const auto stock_cursor = word(m, 0x800ac650);
  put(m, 0x8009db2c, 0x1234);
  rr64_rider_skin_preview_begin(m, node);
  rr64_rider_skin_preview_end(m);
  check(word(m, 0x800ac650) == stock_cursor && word(m, 0x8009db2c) == 0x1234,
        "unselected preview preserves native cursor and cache");
  for (unsigned slot : {2u, 3u}) {
    auto lower = source;
    lower[4].second = 0x07000000 + (slot == 3 ? 512 : 2048);
    lower[8] = {0xe7000000, 0};
    lower[9] = {0xe7000000, 0};
    for (unsigned i = 0; i < lower.size(); ++i)
      command(m, dl + i * 8, lower[i]);
    const unsigned lower_lod = slot - 1;
    engine::write_u16(m, node + 0x4a + lower_lod * 2, 1);
    put(m, node + 0x7c + lower_lod * 32, 0x80500000 + slot * 0x1000);
    configure(m, 40 + slot);
    const auto lower_list =
        list(m, rr64_rider_skin_actor_list(m, node, dl, lower_lod));
    check(lower_list[3].first == 0xfd500000 &&
              lower_list[3].second >= 0x81000000,
          "complete-body and distant textures preserve native image commands");
    check(lower_list[4].second - lower_list[3].second ==
              (slot == 3 ? 512u : 2048u),
          "lower LOD palette offsets follow native texture dimensions");
    check(m[(lower_list[3].second & 0x7fffffff) ^ 3] == 17 + slot,
          "each lower LOD has its corresponding authored texture");
  }
  // Captured native LOD1 and LOD2 share a 64x32 body material. LOD2 has
  // 32x16 UVs, with no compensating tile shift. Exercise that exact binding
  // rather than assuming the engine selects the authored far resource.
  std::vector<MaterialCommand> far_source(source.begin(), source.end());
  far_source[8] = {0xe7000000, 0};
  far_source[9] = {0xe7000000, 0};
  far_source.insert(far_source.begin(), {{0xd7000002, 0x80008000},
                                         {0xf5481000, 0x00014060},
                                         {0xf2000000, 0x000fc07c}});
  bool captured_far = false;
  if (argc == 2) {
    std::ifstream input(argv[1], std::ios::binary);
    std::vector<unsigned char> capture(0x800000);
    input.read(reinterpret_cast<char *>(capture.data()), capture.size());
    check(input.gcount() == static_cast<std::streamsize>(capture.size()),
          "complete private native capture");
    check(word(capture.data(), 0x8024ab90 + 0x9c) ==
              word(capture.data(), 0x8024ab90 + 0xbc),
          "recorded near/far actor material headers are identical");
    far_source = list(capture.data(), 0x8063ccc8);
    check(far_source.size() < 1024,
          "actual native far command list is bounded");
    captured_far = true;
  }
  for (unsigned gender = 0; gender < 2; ++gender) {
    selected = gender + 1;
    for (unsigned lod : {1u, 2u}) {
      engine::write_u16(m, node + 0x4a + lod * 2, 1);
      put(m, node + 0x7c + lod * 32, 0x80502000 + (gender ? 10 : 0) * 0x4000);
    }
    for (unsigned i = 0; i < far_source.size(); ++i)
      command(m, dl + i * 8, far_source[i]);
    configure(m, 50 + gender);
    const auto mid = list(m, rr64_rider_skin_actor_list(m, node, dl, 1));
    const auto far = list(m, rr64_rider_skin_actor_list(m, node, dl, 2));
    unsigned far_image = 0, far_palette = 0, mid_image = 0;
    for (unsigned i = 0; i + 1 < far_source.size(); ++i) {
      if (far_source[i].first == 0xfd500000 &&
          far_source[i].second == 0x07000000) {
        far_image = far[i].second;
        mid_image = mid[i].second;
      } else if (far_source[i].first == 0xfd100000 &&
                 far_source[i].second == 0x07000800) {
        far_palette = far[i].second;
      } else {
        check(far[i] == far_source[i] && mid[i] == far_source[i],
              "actual native far geometry, halfscale and tile commands stay "
              "unchanged");
      }
    }
    check(far_image >= 0x81000000 && far_palette - far_image == 2048 &&
              far_image != mid_image,
          "LOD2 uses padded far skin within original native 64x32 material");
    check(m[(mid_image & 0x7fffffff) ^ 3] == 19 + gender * 32,
          "LOD1 still uses the ordinary complete-body skin");
    for (unsigned y = 0; y < 32; ++y)
      for (unsigned x = 0; x < 64; ++x)
        check(
            m[((far_image & 0x7fffffff) + y * 64 + x) ^ 3] ==
                static_cast<unsigned char>(20 + gender * 32 + std::min(x, 31u) +
                                           std::min(y, 15u) * 5),
            "far texels retain exact 32x16 layout with clamped filter padding");
  }
  check(allocations == 3, "far staging needs no additional render allocation");
  selected = 4;
  configure(m, 30);
  for (unsigned i = 0; i < source.size(); ++i)
    command(m, dl + i * 8, source[i]);
  engine::write_u16(m, node + 0x4a, 2);
  put(m, node + 0x7c, 0x80500000);
  put(m, node + 0x80, 0x80501000);
  const auto shell = list(m, rr64_rider_skin_actor_list(m, node, dl, 0));
  check(shell.size() == source.size() + 7 && shell[11].first == 0x01009012,
        "shell adds nine vertices and eight triangles after native torso");
  check(shell[11].second >= 0x81000000 && allocations == 4,
        "shell uses one immutable extended vertex allocation");
  check(std::int16_t(word(m, shell[11].second) >> 16) == -43 &&
            (word(m, shell[11].second + 4) >> 16) == 8,
        "shell coordinates are written in extended memory, not silently rejected by 8 MiB helpers");
  const auto again = list(m, rr64_rider_skin_actor_list(m, node, dl, 0));
  check(again == shell && allocations == 4, "shell vertices reuse storage without pose mutation");
  selected = 5;
  configure(m, 32);
  constexpr unsigned vertices = 0x80600100;
  const std::array<MaterialCommand, 7> head_source{{
      {0xda380002, 0x06000000}, {0xfd500000, 0x07000000},
      {0xfd100000, 0x07000800}, {0x0100600c, 0x05000100},
      {0x06000204, 0x0006080a}, {0xda380002, 0x06000040}, {0xdf000000, 0}}};
  for (unsigned i = 0; i < 6; ++i) {
    const int y = -20 + (i % 3 == 0 ? 0 : i < 3 ? 7 : -7);
    put(m, vertices + i * 16, (unsigned(i % 3 == 0 ? 33 : 25) << 16) | std::uint16_t(y));
    put(m, vertices + i * 16 + 4, 27u << 16);
    put(m, vertices + i * 16 + 8, (unsigned(i % 3 == 0 ? 26 : 38) * 64 << 16) | (27 * 64));
    put(m, vertices + i * 16 + 12, 0x7f0000ff);
  }
  for (unsigned i = 0; i < head_source.size(); ++i)
    command(m, dl + i * 8, head_source[i]);
  check(rr64_rider_skin_actor_list(m, node, dl, 0) == dl,
        "asymmetric face refuses an unresolved native segment");
  const auto face = list(m, rr64_rider_skin_actor_list(m, node, dl, 0, 0x80600000));
  std::array<unsigned, 2> halves{};
  unsigned faces = 0;
  for (const auto &c : face)
    if (c.first == 0x01003006) {
      check(faces < halves.size(), "exactly two remapped face triangles");
      halves[faces++] = word(m, c.second + 8) >> 16;
      check(word(m, c.second) == ((33u << 16) | std::uint16_t(-20)), "translated head position unchanged");
      check((word(m, c.second + 8) & 0xffff) == 27 * 64,
            "live face retains vertical UVs even outside reference XYZ bounds");
    }
  check(faces == 2 && halves[0] - halves[1] == 19 * 64,
        "opposite cheeks sample distinct half-head strips including center vertices");
  check(std::count(face.begin(), face.end(), head_source[3]) == 2 && allocations == 4,
        "original vertex batch restored before next bone without extra allocations");
  check((word(m, vertices + 8) >> 16) == 26 * 64,
        "shared native head UVs remain unchanged");
  selected = 1;
  for (unsigned i = 0; i < source.size(); ++i)
    command(m, dl + i * 8, source[i]);
  engine::write_u16(m, node + 0x4a, 2);
  // Freed resources and non-rider/disabled identities must never tint peers.
  configure(m, 31);
  put(m, node + 0x7c, 0x80500000);
  put(m, node + 0x80, 0x80501000);
  put(m, 0x800d3840, 0);
  put(m, 0x800d3848, 0);
  check(rr64_rider_skin_actor_list(m, node, dl, 0) == dl,
        "unloaded donor resources refuse stale pointers");
  selected = 0;
  check(rr64_rider_skin_actor_call(m, node, dl, call, 0) == dl,
        "stock actor preserves original call without effects");
  selected = 1;
  check(rr64_rider_skin_actor_list(m, node, dl, 3) == dl,
        "invalid LOD refused");
  const auto saved = word(m, call);
  put(m, 0x800ac650, call + 16);
  check(rr64_rider_skin_actor_call(m, node, dl, call, 0) == dl &&
            word(m, call) == saved,
        "invalid caller cursor never partially rewrites");
  std::printf("Rider skin render: %u checks (%u native donor/LOD/effect calls, "
              "captured far=%u) "
              "passed\n",
              checks, scenarios, unsigned(captured_far));
}
