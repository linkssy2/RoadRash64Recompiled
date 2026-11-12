#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include "rr64_netplay.hpp"

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: RR64NetplayCapacity host|join port duration-ms peer-id\n");
        return 2;
    }

    const std::string mode = argv[1];
    const bool host = mode == "host";
    const bool join = mode == "join";
    const long parsed_port = std::strtol(argv[2], nullptr, 10);
    const long duration_ms = std::strtol(argv[3], nullptr, 10);
    const long peer_id = std::strtol(argv[4], nullptr, 10);
    if ((!host && !join) || parsed_port < 1024 || parsed_port > 65535 ||
        duration_ms < 1000 || duration_ms > 30000 || peer_id < 0 || peer_id > 255) {
        return 2;
    }

    rr64::netplay::Config config{};
    config.mode = host ? rr64::netplay::Mode::Host : rr64::netplay::Mode::Join;
    config.player_name = "CapacityPeer" + std::to_string(peer_id);
    config.host_address = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(parsed_port);
    rr64::netplay::configure(config);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(duration_ms);
    std::uint8_t peak_players = 0;
    std::uint8_t peak_riders = 0;
    bool admitted = false;
    bool canonical_snapshot_seen = false;
    std::uint8_t assigned_slot = rr64::netplay::kInvalidSlot;
    while (std::chrono::steady_clock::now() < deadline) {
        rr64::netplay::update();
        const rr64::netplay::Status status = rr64::netplay::get_status();
        peak_players = std::max(peak_players, status.connected_players);
        if (status.connected) {
            admitted = true;
            assigned_slot = status.local_slot;
            rr64::netplay::set_ready(true);
        }
        if (host && status.connected_players == rr64::netplay::kMaximumPlayers &&
            rr64::netplay::all_connected_players_ready() && status.phase == rr64::netplay::Phase::Lobby) {
            rr64::netplay::host_set_phase(rr64::netplay::Phase::GameSetup);
        }
        if (host && status.phase == rr64::netplay::Phase::GameSetup) {
            rr64::netplay::GameSetupState setup{};
            setup.valid = true;
            setup.transition_buttons = 0x8000u;
            for (std::size_t i = 0; i < setup.words.size(); ++i) {
                setup.words[i] = static_cast<std::uint32_t>(peer_id * 100 + static_cast<long>(i));
            }
            rr64::netplay::host_commit_game_setup(setup);
        }
        if (status.phase == rr64::netplay::Phase::CharacterSelect || status.phase == rr64::netplay::Phase::TrackSelect) {
            if(!rr64::netplay::acknowledge_course(status.game_setup.revision,status.game_setup.course))return 5;
            const rr64::online_flow::Selection selection{status.game_setup.revision,
                unsigned(status.local_slot)+1,unsigned(status.local_slot)%4,1,
                status.phase==rr64::netplay::Phase::TrackSelect?1u:0u};
            if(!rr64::netplay::set_selection(selection))return 6;
            if(host && status.phase==rr64::netplay::Phase::TrackSelect)
                rr64::netplay::host_request_race_start();
            if(host)rr64::netplay::host_release_selection();
        }
        const rr64::netplay::Status race_status = rr64::netplay::get_status();
        if (race_status.phase == rr64::netplay::Phase::Race &&
            race_status.local_slot < rr64::netplay::kMaximumPlayers) {
            rr64::netplay::RiderState rider{};
            rider.active = true;
            rider.position_x = static_cast<float>(race_status.local_slot) * 100.0f + 1.0f;
            rider.position_y = static_cast<float>(race_status.local_slot) * 100.0f + 2.0f;
            rider.position_z = static_cast<float>(race_status.local_slot) * 100.0f + 3.0f;
            rider.front_wheel_x = static_cast<float>(race_status.local_slot) + 0.25f;
            rider.rear_wheel_x = static_cast<float>(race_status.local_slot) * 0.1f;
            rider.health = static_cast<std::uint16_t>(1000u - race_status.local_slot);
            rr64::netplay::set_local_rider_state(rider);

            std::uint8_t synchronized = 0;
            for (std::uint8_t slot = 0; slot < rr64::netplay::kMaximumPlayers; ++slot) {
                rr64::netplay::RiderState observed{};
                if (rr64::netplay::get_rider_state(slot, observed) &&
                    observed.position_x == static_cast<float>(slot) * 100.0f + 1.0f &&
                    observed.health == static_cast<std::uint16_t>(1000u - slot)) {
                    ++synchronized;
                }
            }
            peak_riders = std::max(peak_riders, synchronized);
            canonical_snapshot_seen = canonical_snapshot_seen || synchronized == rr64::netplay::kMaximumPlayers;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    const rr64::netplay::Status final_status = rr64::netplay::get_status();
    rr64::netplay::shutdown();
    if (host) {
        std::fprintf(
            stderr,
            "[RR64-CAPACITY] host peak=%u final=%u protocol-limit=%u canonical-riders=%u\n",
            static_cast<unsigned>(peak_players),
            static_cast<unsigned>(final_status.connected_players),
            static_cast<unsigned>(rr64::netplay::kMaximumPlayers),
            static_cast<unsigned>(peak_riders));
        return peak_players == rr64::netplay::kMaximumPlayers && canonical_snapshot_seen ? 0 : 1;
    }

    std::fprintf(
        stderr,
        "[RR64-CAPACITY] peer=%ld admitted=%u slot=%u peak-seen=%u canonical-riders=%u\n",
        peer_id,
        admitted ? 1u : 0u,
        static_cast<unsigned>(assigned_slot),
        static_cast<unsigned>(peak_players),
        static_cast<unsigned>(peak_riders));
    return admitted && canonical_snapshot_seen ? 0 : 4;
}
