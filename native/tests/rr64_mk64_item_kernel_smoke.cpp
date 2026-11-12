#include "rr64_mk64_item_kernel.hpp"
#include "rr64_mk64_item_dimensions.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace rr64::mk64_items;
namespace {
unsigned checks = 0;
void require(bool condition, const char *description) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "FAIL %s at check %u\n", description, checks);
    std::exit(1);
  }
}
std::array<Racer, racer_capacity> poses() {
  std::array<Racer, racer_capacity> p{};
  for (unsigned i = 0; i < p.size(); ++i) {
    p[i] = {
        true,       true,      false, i < 4,         {float(i) * 4, 0, .15f},
        {0, 10, 0}, {0, 1, 0}, .45f,  float(i) * 10, 25};
    // Generic test scene; captured native compound shapes have a separate
    // fixture.
    p[i].contacts[0] = {{0, 0, .4f}, .45f};
    p[i].contact_count = 1;
  }
  return p;
}
unsigned objects(const Snapshot &s) {
  unsigned count = 0;
  for (const auto &o : s.objects)
    count += o.generation != 0;
  return count;
}
SurfaceHit floor(void *, Vec start, Vec motion, float radius) {
  if (start[2] < radius)
    return {true, 0, {0, 0, 1}, {start[0], start[1], 0}, radius - start[2]};
  if (motion[2] < 0 && start[2] + motion[2] < radius) {
    const float t = std::clamp((radius - start[2]) / motion[2], 0.f, 1.f);
    return {true,
            t,
            {0, 0, 1},
            {start[0] + motion[0] * t, start[1] + motion[1] * t, 0}};
  }
  return {};
}
SurfaceHit wall(void *ctx, Vec start, Vec motion, float radius) {
  const auto ground = floor(ctx, start, motion, radius);
  if (motion[1] > 0 && start[1] + motion[1] > 3 - radius &&
      start[1] <= 3 - radius) {
    const float t = (3 - radius - start[1]) / motion[1];
    if (ground.hit && ground.fraction < t)
      return ground;
    return {true,
            t,
            {0, -1, 0},
            {start[0] + motion[0] * t, 3 - radius, start[2] + motion[2] * t}};
  }
  return ground;
}
StepResult use_item(Snapshot &s, std::array<Racer, racer_capacity> &p,
                    unsigned slot, unsigned clock = 10) {
  std::array<Use, racer_capacity> input{};
  input[slot].pressed = true;
  return step(s, p, input, clock, 1.f / 60, {nullptr, floor, nullptr});
}
void advance(Snapshot &s, std::array<Racer, racer_capacity> &p, unsigned end) {
  std::array<Use, racer_capacity> no{};
  for (unsigned t = s.clock + 1; t <= end; ++t) {
    step(s, p, no, t, 1.f / 30, {nullptr, floor, nullptr});
    require(valid(s), "advanced state valid");
  }
}
} // namespace
int main() {
  static_assert(sizeof(Snapshot) == 3504);
  for (Item kind : {Item::GreenShell, Item::RedShell, Item::BlueShell}) {
    // A straight swept path through a 0.45-unit native-sized sphere. The old
    // 1.15-radius shell hit at lateral1.45; the reduced shell must miss there.
    for (bool grazing : {false, true}) {
      auto racers = poses();
      for (unsigned i = 2; i < racers.size(); ++i) racers[i].active = false;
      racers[0].position = {-20, -20, 2};
      racers[1].position = {grazing ? 1.45f : 0.f, 0, 2};
      racers[1].velocity = {};
      racers[1].contacts[0] = {{}, .45f};
      Snapshot state;
      initialize(state, 72, 10);
      state.next_generation = 1;
      state.objects[0] = {1, 10, 460, kind, ObjectMode::Flying, 0, no_target,
                          0, 0, 0, 0, {0, -2, 2}, {0, 20, 3}};
      std::array<Use, racer_capacity> none{};
      const auto straight = [](void *, Vec position, Vec) -> Vec {
        return {position[0], position[1] + 100, position[2]};
      };
      const auto result = step(state, racers, none, 11, .2f, {nullptr, nullptr, straight});
      require(result.hits[1].active != grazing,
              "reduced green/red/blue shell retains center hits and rejects old grazing contact");
      require(bool(state.objects[0].generation) == grazing,
              "only an accepted smaller-shell hit consumes the projectile");
    }
  }
  for (Item held : {Item::GreenShell, Item::RedShell, Item::BlueShell,
                    Item::TripleGreenShell, Item::TripleRedShell}) {
    auto racers = poses();
    for (unsigned i = 1; i < racers.size(); ++i) racers[i].active = false;
    Snapshot state;
    initialize(state, 73, 10);
    require(grant(state, 0, held), "shell spacing inventory grant");
    use_item(state, racers, 0);
    const bool triple = held == Item::TripleGreenShell || held == Item::TripleRedShell;
    for (const auto &object : state.objects) if (object.generation) {
      require(object_radius(object.kind) == .8625f, "every deployed shell uses the reduced collision radius");
      const float x = object.position[0] - racers[0].position[0];
      const float y = object.position[1] - racers[0].position[1];
      if (triple) {
        require(std::abs(std::hypot(x, y) - 1.4125f) < .0001f,
                "triple orbit clearance follows smaller shell size");
      } else {
        require(std::abs(y - 1.4125f - 22.f / 60) < .0001f,
                "single-shell launch clearance follows smaller shell size");
      }
    }
    if (triple) {
      use_item(state, racers, 0, 15);
      bool released = false;
      for (const auto &object : state.objects) if (object.generation && object.mode == ObjectMode::Flying) {
        released = true;
        require(std::abs(object.position[1] - racers[0].position[1] - 1.4125f - 22.f / 60) < .0001f,
                "released triple shell shares reduced launch clearance");
      }
      require(released, "triple release produces its normal shell projectile");
    }
  }
  require(object_radius(Item::Banana) == .35f && object_radius(Item::FakeBox) == 1.1f,
          "shell reduction does not change banana or fake-box collision size");
  for (const bool stationary : {false, true}) {
    Snapshot state;
    initialize(state, 71, 10);
    const Item kind = stationary ? Item::FakeBox : Item::GreenShell;
    state.next_generation = 1;
    state.objects[0] = {1, 10, 460, kind,
        stationary ? ObjectMode::Resting : ObjectMode::Flying, 0, no_target,
        0, 0, 0, 0, {0, 0, 2}, stationary ? Vec{} : Vec{20, 0, 3}};
    std::array<Racer, racer_capacity> racers{};
    racers[0].active = racers[0].riding = true;
    racers[0].position = {-20, 0, 2};
    auto &victim = racers[1];
    victim.active = victim.riding = true;
    victim.position = {stationary ? 2.f : 3.5f, .8f, 2};
    victim.velocity = {stationary ? 20.f : 10.f, 0, 0};
    victim.contacts[0] = {{}, .45f};
    victim.contact_count = 1;
    std::array<Use, racer_capacity> none{};
    const auto result = step(state, racers, none, 11, .2f, {});
    const auto &hit = result.hits[1];
    require(hit.active && !state.objects[0].generation,
            "swept moving victim contact survives object retirement");
    require(hit.surface_velocity == (stationary ? Vec{} : Vec{20, 0, 0}),
            "impact retains raw moving shell or stationary box surface velocity");
    const float combined_radius = stationary ? 1.55f : 1.3125f;
    const float expected_y = .8f / combined_radius;
    const float expected_x = std::sqrt(1 - expected_y * expected_y) * (stationary ? -1.f : 1.f);
    require(std::abs(hit.direction[1] - expected_y) < .0001f &&
                std::abs(hit.direction[0] - expected_x) < .0001f,
            "impact normal uses off-center sphere contact, not projectile travel");
    require(hit.victim_displacement[0] > 1,
            "moving victim regression spans appreciable remaining frame travel");
    for (unsigned axis = 0; axis < 3; ++axis)
      require(std::abs(hit.point[axis] + hit.victim_displacement[axis] -
                       victim.position[axis] + hit.direction[axis] * .45f) < .0001f,
              "translated impact point retains contact-time native torque lever");
  }
  auto p = poses();
  for (unsigned id = 1; id <= 15; ++id) {
    Snapshot s;
    initialize(s, 123);
    require(grant(s, 0, Item(id)), "all original item ids grant");
    require(!grant(s, 0, Item::Banana), "occupied inventory never overwritten");
    require(valid(s), "grant validates");
    use_item(s, p, 0);
    require(valid(s), "every item use produces valid state");
    const auto after = s;
    use_item(s, p, 0);
    require(s.riders[0].charges == after.riders[0].charges,
            "same-clock use cooldown");
    advance(s, p, 1850);
    require(!s.riders[0].star_until && !s.riders[0].boo_until &&
                !s.riders[0].shrink_until && !s.riders[0].boost_until,
            "temporary effects expire");
    retire(s, 0);
    require(!objects(s), "actor retirement removes owned objects");
  }
  for (Item item :
       {Item::TripleGreenShell, Item::TripleRedShell, Item::BananaBunch}) {
    Snapshot s;
    initialize(s, 12);
    grant(s, 0, item);
    use_item(s, p, 0);
    const unsigned count = item == Item::BananaBunch ? 5 : 3;
    require(objects(s) == count && s.riders[0].deployed,
            "stack deploys distinct defenses");
    for (unsigned n = 0; n < count; ++n) {
      use_item(s, p, 0, 15 + n * 5);
      require(valid(s), "stack release validates");
      require(s.riders[0].charges == count - n - 1, "stack uses one charge");
    }
    require(s.riders[0].held == Item::None, "spent stack cleared");
  }
  {
    Snapshot s;
    initialize(s, 1);
    grant(s, 0, Item::Boo);
    grant(s, 1, Item::TripleMushroom);
    use_item(s, p, 0);
    require(s.riders[0].held == Item::TripleMushroom &&
                s.riders[0].charges == 3 && s.riders[1].held == Item::None &&
                s.riders[0].boo_until == 220,
            "Boo transfers one actual stack");
    require(valid(s), "Boo transfer validates");
    grant(s, 2, Item::Lightning);
    const auto hits = use_item(s, p, 2, 15);
    require(!hits.hits[0].active && !s.riders[0].shrink_until,
            "Boo resists lightning");
  }
  {
    Snapshot s;
    initialize(s, 3);
    grant(s, 0, Item::Star);
    use_item(s, p, 0);
    grant(s, 1, Item::Lightning);
    const auto hits = use_item(s, p, 1, 15);
    require(!hits.hits[0].active && !s.riders[0].shrink_until,
            "Star resists lightning");
    require(!hits.hits[2].active && !s.riders[2].hit_until && s.riders[2].shrink_until == 315,
            "lightning shrinks rivals without an immediate crash or hit cooldown");
    StepResult contact;
    runover(s, contact, 2, 1, p[2].position, p[1].velocity);
    require(contact.hits[2].active && contact.hits[2].owner == 1,
            "later native contact retains the larger rider as canonical attacker");
    const auto once = s;
    StepResult repeated;
    runover(s, repeated, 2, 1, p[2].position, p[1].velocity);
    require(!repeated.hits[2].active && s == once, "runover hit cooldown prevents duplicate contact");
    for (unsigned blocker = 0; blocker < 6; ++blocker) {
      Snapshot guarded;
      initialize(guarded, 1, 20);
      guarded.riders[2].shrink_until = blocker == 0 ? 20 : 300;
      if (blocker == 1) guarded.riders[1].shrink_until = 300;
      if (blocker == 2) guarded.riders[2].star_until = 300;
      if (blocker == 3) guarded.riders[2].boo_until = 300;
      if (blocker == 4) guarded.riders[1].boo_until = 300;
      if (blocker == 5) guarded.enabled = 0;
      StepResult rejected;
      runover(guarded, rejected, 2, 1, p[2].position, p[1].velocity);
      require(!rejected.hits[2].active, "expired/equal-size/Star/Boo/OFF runover is rejected");
    }
  }
  {
    Snapshot s;
    initialize(s, 9);
    grant(s, 0, Item::GoldenMushroom);
    use_item(s, p, 0);
    for (unsigned t = 15; t < 235; t += 5) {
      p[0].velocity[1] += 10;
      use_item(s, p, 0, t);
    }
    require(s.riders[0].boost_speed == 33.75f,
            "golden boost never compounds current velocity");
    advance(s, p, 240);
    require(s.riders[0].held == Item::None, "golden stack expires");
    p = poses();
  }
  {
    Snapshot s;
    initialize(s, 12);
    grant(s, 0, Item::GreenShell);
    auto sparse = p;
    for (unsigned i = 1; i < sparse.size(); ++i)
      sparse[i].active = false;
    std::array<Use, racer_capacity> input{};
    input[0].pressed = true;
    step(s, sparse, input, 10, .05f, {nullptr, wall, nullptr});
    input = {};
    step(s, sparse, input, 12, .05f, {nullptr, wall, nullptr});
    bool bounce = false;
    for (const auto &o : s.objects)
      bounce |= o.generation && o.bounces && o.velocity[1] < 0;
    require(bounce, "green shell reflects at actual surface");
    require(valid(s), "bounced shell validates");
  }
  {
    Snapshot s;
    initialize(s, 15);
    auto aligned = poses();
    for (unsigned i = 2; i < aligned.size(); ++i)
      aligned[i].active = false;
    aligned[1].position = {0, 2, .15f};
    aligned[1].velocity = {};
    grant(s, 0, Item::GreenShell);
    std::array<Use, racer_capacity> input{};
    input[0].pressed = true;
    const auto hits =
        step(s, aligned, input, 10, .08f, {nullptr, floor, nullptr});
    require(hits.hits[1].active && hits.hits[1].item == Item::GreenShell,
            "swept shell hits between frames");
    require(hits.hits[1].owner == 0 && !objects(s),
            "single shell consumed on accepted collision");
  }
  {
    Snapshot s;
    initialize(s, 17);
    grant(s, 0, Item::TripleGreenShell);
    use_item(s, p, 0);
    auto bad = s;
    bad.objects[1].orbit = bad.objects[0].orbit;
    require(!valid(bad), "duplicate orbital slot rejected");
    bad = s;
    bad.objects[0].position[0] = std::numeric_limits<float>::quiet_NaN();
    require(!valid(bad), "nonfinite object rejected");
    bad = s;
    bad.riders[0].star_until = s.clock;
    require(!valid(bad), "expired nonzero timer rejected");
    bad = s;
    bad.objects[0].owner = 14;
    require(!valid(bad), "out of bounds owner rejected");
  }
  {
    // A diagonal wall reflection has two collision legs, not the chord
    // joining their endpoints. Test both legs and a rider behind the wall.
    auto racers = poses();
    for (auto &r : racers)
      r.active = false;
    racers[0] = p[0];
    racers[0].position = {-10, -10, .15f};
    racers[0].velocity = {};
    racers[1] = p[1];
    racers[1].velocity = {};
    racers[1].radius = .1f;
    racers[1].contacts[0].radius = .1f;
    const float shell_radius = object_radius(Item::GreenShell);
    const float rider_z = shell_radius - .3f; // Native test sphere offset is +.4.
    const float reflected_y = 2 * (3 - shell_radius) - 3.5f;
    const auto run = [&](Vec position) {
      Snapshot s;
      initialize(s, 20, 10);
      s.next_generation = 1;
      s.objects[0] = {
          1, 10, 460, Item::GreenShell, ObjectMode::Flying, 0, no_target, 0,
          0, 0,  0,   {0, 0, shell_radius + .1f},     {20, 20, 0}};
      racers[1].position = position;
      std::array<Use, racer_capacity> none{};
      return step(s, racers, none, 11, .2f, {nullptr, wall, nullptr});
    };
    require(run({1.f, 1.f, rider_z}).hits[1].active,
            "rider hit before wall reflection");
    require(run({3.5f, reflected_y, rider_z}).hits[1].active,
            "rider hit along reflected segment");
    require(!run({2.8f, 4.4f, rider_z}).hits[1].active,
            "wall blocks rider contact behind it");
  }
  {
    auto racers = poses();
    for (unsigned i = 2; i < racers.size(); ++i)
      racers[i].active = false;
    racers[0].velocity = {0, 190, 0};
    racers[0].maximum_speed = 141;
    racers[1].position = {0, 50, .15f};
    racers[1].velocity = {0, 200, 0};
    racers[1].maximum_speed = 141;
    racers[1].progress = 100;
    Snapshot s;
    initialize(s, 43);
    grant(s, 0, Item::BlueShell);
    use_item(s, racers, 0);
    bool catches = false;
    for (const auto &o : s.objects)
      if (o.generation)
        catches = o.velocity[1] > 200 && o.velocity[1] <= 400;
    require(
        catches,
        "homing shell can catch boosted Insanity bike within bounded speed");
    require(valid(s),
            "Insanity shell speed validates for network transmission");
  }
  {
    auto racers = poses();
    for (unsigned i = 1; i < racers.size(); ++i)
      racers[i].active = false;
    Snapshot s;
    initialize(s, 19, 10);
    s.next_generation = 1;
    s.objects[0] = {
        1, 10, 460, Item::GreenShell, ObjectMode::Flying, 0, no_target, 0,
        0, 0,  0,   {0, 0, .1f},      {0, 10, 0}};
    std::array<Use, racer_capacity> none{};
    step(s, racers, none, 11, .1f, {nullptr, floor, nullptr});
    require(s.objects[0].generation && s.objects[0].position[2] > .44f,
            "initial floor penetration separates shell to actual surface");
    require(s.objects[0].position[1] > .9f,
            "floor overlap does not stall horizontal travel");
  }
  {
    Snapshot a, b;
    initialize(a, 300);
    initialize(b, 300);
    auto racers = poses();
    unsigned rng = 57;
    std::array<Use, racer_capacity> input{};
    for (unsigned tick = 1; tick <= 3600; ++tick) {
      for (unsigned slot = 0; slot < racer_capacity; ++slot) {
        rng = rng * 1664525u + 1013904223u;
        if (rng % 13 == 0) {
          Item item = Item(1 + (rng >> 8) % 15);
          grant(a, slot, item);
          grant(b, slot, item);
        }
        input[slot] = {rng % 5 == 0, std::int8_t((rng % 3 - 1) * 50)};
      }
      step(a, racers, input, tick, 1.f / 30, {nullptr, floor, nullptr});
      step(b, racers, input, tick, 1.f / 30, {nullptr, floor, nullptr});
      require(a == b, "same input/seed deterministic");
      require(valid(a), "14 racer mixed item soak validates");
    }
  }
  std::printf("MK64 item kernel: %u checks passed\n", checks);
}
