#pragma once

#include <cstdint>

namespace rr64::engine {

enum class ContractHealth : std::uint8_t {
    Unobserved = 0,
    Healthy,
    Warning,
};

struct ContractSnapshot {
    ContractHealth health = ContractHealth::Unobserved;
    std::uint64_t observed_frames = 0;
    std::uint32_t main_mode = 0;
    std::uint32_t multiplayer_stage = 0;
    std::uint32_t active_racers = 0;
    std::uint32_t bike_pool = 0;
    float physics_delta = 0.0f;
};

void observe_frame(unsigned char *rdram) noexcept;
void capture_pre_update(unsigned char *rdram) noexcept;
void capture_post_update(unsigned char *rdram) noexcept;
void capture_dispatch_boundary(unsigned char *rdram, std::uint32_t boundary,
                               std::uint32_t target) noexcept;
void capture_race_update_boundary(unsigned char *rdram, std::uint32_t boundary) noexcept;
void capture_dynamics_boundary(unsigned char *rdram, std::uint32_t function_id,
                               std::uint32_t boundary, std::uint32_t actor_address) noexcept;
void capture_dynamics_auxiliary_boundary(unsigned char *rdram, std::uint32_t function_id,
                                         std::uint32_t boundary, std::uint32_t actor_address,
                                         std::uint32_t auxiliary_0, std::uint32_t auxiliary_1,
                                         std::uint32_t auxiliary_2, std::uint32_t auxiliary_3,
                                         std::uint32_t auxiliary_4) noexcept;
void capture_dynamics_auxiliary8_boundary(unsigned char *rdram, std::uint32_t function_id,
                                          std::uint32_t boundary, std::uint32_t actor_address,
                                          std::uint32_t auxiliary_0, std::uint32_t auxiliary_1,
                                          std::uint32_t auxiliary_2, std::uint32_t auxiliary_3,
                                          std::uint32_t auxiliary_4, std::uint32_t auxiliary_5,
                                          std::uint32_t auxiliary_6,
                                          std::uint32_t auxiliary_7) noexcept;
void capture_actor_presentation(unsigned char *rdram, std::uint32_t viewport) noexcept;
ContractSnapshot contract_snapshot() noexcept;

} // namespace rr64::engine
