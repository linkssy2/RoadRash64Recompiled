#pragma once

#include <cstddef>
#include <cstdint>

namespace rr64::achievements::logic {

// The first four entries in the local achievement table are the Big Game
// level promotions. Keeping this mapping independent of the UI/persistence
// code lets the ROM-free smoke test verify every accepted boundary.
inline constexpr std::size_t kAchievementCount = 52;
inline constexpr std::size_t kInvalidAchievement = kAchievementCount;

constexpr std::size_t campaign_level_index(std::uint32_t new_level) {
    return new_level >= 1 && new_level <= 4 ? static_cast<std::size_t>(new_level - 1)
                                            : kInvalidAchievement;
}

inline constexpr std::size_t kCampaignCompletionIndex = 4;

} // namespace rr64::achievements::logic
