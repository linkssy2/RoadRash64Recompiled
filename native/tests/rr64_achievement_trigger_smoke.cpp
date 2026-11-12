#include <array>
#include <cstdint>
#include <cstdio>

#include "rr64_achievement_logic.hpp"

namespace {

bool expect_equal(std::size_t actual, std::size_t expected, const char* label) {
    if (actual == expected) {
        return true;
    }

    std::fprintf(
        stderr,
        "[RR64-ACH-SMOKE] %s: expected %zu, got %zu\n",
        label,
        expected,
        actual);
    return false;
}

} // namespace

int main() {
    using namespace rr64::achievements::logic;

    bool passed = true;
    constexpr std::array<std::size_t, 4> expected{0, 1, 2, 3};
    for (std::uint32_t level = 1; level <= 4; ++level) {
        passed &= expect_equal(
            campaign_level_index(level),
            expected[level - 1],
            "campaign promotion");
    }

    passed &= expect_equal(campaign_level_index(0), kInvalidAchievement, "level zero");
    passed &= expect_equal(campaign_level_index(5), kInvalidAchievement, "completion boundary");
    passed &= expect_equal(
        campaign_level_index(UINT32_MAX),
        kInvalidAchievement,
        "invalid large level");
    passed &= expect_equal(kCampaignCompletionIndex, 4, "campaign completion");

    if (!passed) {
        return 1;
    }

    std::puts("RR64 achievement trigger smoke test passed.");
    return 0;
}
