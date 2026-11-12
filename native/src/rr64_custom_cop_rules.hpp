#pragma once

namespace rr64::custom_cop {
// Stock USA v1.0 Cop Mode bike and police rider profiles (40..44). Keep the
// equipment AND-rule independent of controller slot and ordinary unlocks.
constexpr bool selected_cop(bool enabled, unsigned bike, unsigned rider) {
    return enabled && bike == 31 && (rider >= 40 && rider <= 44);
}
} // namespace rr64::custom_cop
