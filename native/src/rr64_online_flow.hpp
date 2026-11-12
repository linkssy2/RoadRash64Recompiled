#pragma once
#include <cstdint>

namespace rr64::online_flow {
// Persistent host commands for native multiplayer Results -> Statistics ->
// Race Setup. A missed edge or a slower highlight reel cannot strand a peer.
struct PostRaceState {
    std::uint32_t round = 0;
    std::uint32_t stage = 0; // 1: statistics (1F), 2: next setup (23)
    bool operator==(const PostRaceState &) const = default;
};
constexpr bool valid(const PostRaceState &s) {
    return (!s.round && !s.stage) || (s.round && s.stage >= 1 && s.stage <= 2);
}
constexpr bool advances(PostRaceState next, PostRaceState previous) {
    return valid(next) && (next == previous || next.round > previous.round ||
        (next.round == previous.round && next.stage > previous.stage));
}
constexpr unsigned postrace_stage(unsigned from, unsigned to) {
    return from == 0x1e && to == 0x1f ? 1u : from == 0x1f && to == 0x23 ? 2u : 0u;
}
// Swapping local and host slots is its own inverse. Only the >4-player
// representation uses guest rider zero for every machine's local player.
constexpr unsigned mapped_slot(unsigned slot, unsigned local, bool replicated) {
    if (!replicated) return slot;
    return slot == local ? 0u : slot == 0 ? local : slot;
}
// Confirmed choices are round-scoped state, never reconstructed from delayed
// controller edges. The host locks the roster before allowing any race load.
struct Selection {
    std::uint32_t round = 0;
    std::uint32_t rider = 0;
    std::uint32_t bike = 0;
    std::uint32_t confirmed = 0;
    std::uint32_t loaded = 0;
    bool operator==(const Selection &) const = default;
};
constexpr bool valid(const Selection &s) {
    return s.rider <= 44 && s.bike <= 31 && s.confirmed <= 1 &&
           s.loaded <= 1 && (!s.loaded || s.confirmed);
}
constexpr std::uint16_t race_buttons(std::uint16_t buttons, unsigned owner) {
    return owner == 0 ? buttons : buttons & ~std::uint16_t(0x1000);
}
// A persistent network command must produce fresh controller edges. A held
// Start can be consumed during the selector transition, before the prompt is
// listening. Retry only in TrackSelect; the network readiness barrier remains.
constexpr std::uint16_t start_prompt_buttons(bool requested, std::uint64_t milliseconds) {
    return requested && milliseconds % 300u < 120u ? 0x1000u : 0u;
}
// Physical profile zero controls this machine's network rider, not host slot0.
constexpr unsigned shortcut_slot(unsigned profile,bool online,unsigned local,bool replicated) {
    return online ? mapped_slot(local,local,replicated) : profile;
}
}
