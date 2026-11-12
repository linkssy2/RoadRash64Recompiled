#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <limits>
#include <algorithm>
#include <vector>

#include "rr64_netplay.hpp"

int main(int argc, char** argv) {
    if (argc < 3 || argc > 5) {
        std::fprintf(stderr, "usage: RR64NetplaySmoke host|join port [expected-players] [race-seconds]\n");
        return 2;
    }

    const std::string mode = argv[1];
    const bool host = mode == "host";
    const bool join = mode == "join";
    const long parsed_port = std::strtol(argv[2], nullptr, 10);
    const long expected_players = argc >= 4 ? std::strtol(argv[3], nullptr, 10) : 2;
    const long race_seconds=argc==5 ? std::strtol(argv[4],nullptr,10) : 0;
    if(race_seconds<0 || race_seconds>120) return 2;
    if ((!host && !join) || parsed_port < 1024 || parsed_port > 65535) {
        return 2;
    }
    if (expected_players < 2 || expected_players > rr64::netplay::kMaximumPlayers) {
        return 2;
    }

    rr64::netplay::Config config{};
    config.mode = host ? rr64::netplay::Mode::Host : rr64::netplay::Mode::Join;
    config.player_name = host ? "SmokeHost" : "SmokeClient";
    config.host_address = "127.0.0.1";
    config.port = static_cast<std::uint16_t>(parsed_port);
    rr64::netplay::configure(config);

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20+race_seconds);
    auto race_started=std::chrono::steady_clock::time_point{};
    unsigned motion_frame=0;
    std::array<unsigned,14> observed_tick{},observed_updates{};
    std::array<std::chrono::steady_clock::time_point,14> observed_at{};
    std::array<long long,14> maximum_gap_ms{};
    std::array<std::vector<long long>,14> ages_us{},gaps_us{};
    bool ready_sent = false;
    bool passed = false;
    bool remote_voice_received = false;
    auto last_voice_send = std::chrono::steady_clock::time_point{};
    while (std::chrono::steady_clock::now() < deadline) {
        rr64::netplay::update();
        const rr64::netplay::Status status = rr64::netplay::get_status();
        if (status.local_slot < rr64::netplay::kMaximumPlayers) {
            rr64::netplay::set_local_input(
                static_cast<std::uint16_t>(0x5000u | status.local_slot),
                static_cast<float>(status.local_slot) / 4.0f,
                -static_cast<float>(status.local_slot) / 4.0f);
        }
        if (status.connected && !ready_sent) {
            ready_sent = rr64::netplay::set_ready(true);
        }
        if (host && status.connected_players >= expected_players && rr64::netplay::all_connected_players_ready() &&
            status.phase == rr64::netplay::Phase::Lobby) {
            rr64::netplay::host_set_phase(rr64::netplay::Phase::GameSetup);
        }
        if (host && status.phase == rr64::netplay::Phase::GameSetup) {
            rr64::netplay::GameSetupState setup{};
            setup.valid = true;
            setup.transition_buttons = 0x8000u;
            setup.race_options = 512u | (7u<<6) | (3u<<4) | 6u;
            setup.random_seed = 0x12345678;
            for (std::size_t i = 0; i < setup.words.size(); ++i) {
                setup.words[i] = 0x64000000u + static_cast<std::uint32_t>(i);
            }
            rr64::netplay::host_commit_game_setup(setup);
        }
        if (status.phase == rr64::netplay::Phase::CharacterSelect || status.phase == rr64::netplay::Phase::TrackSelect) {
            if (!rr64::netplay::acknowledge_course(status.game_setup.revision,status.game_setup.course)) return 8;
            rr64::online_flow::Selection selection{status.game_setup.revision,
                unsigned(status.local_slot)+1, unsigned(status.local_slot)%4,1,
                status.phase==rr64::netplay::Phase::TrackSelect ? 1u:0u};
            if (!rr64::netplay::set_selection(selection)) return 3;
            auto stale=selection; stale.round=0;
            if (rr64::netplay::set_selection(stale)) return 4;
            if (status.phase==rr64::netplay::Phase::TrackSelect) {
                if (!status.game_setup.start_requested && rr64::netplay::host_release_selection()) return 5;
                const bool start=rr64::netplay::host_request_race_start();
                if (start!=host) return 6;
            }
            rr64::netplay::host_release_selection();
        }
        const rr64::netplay::Status updated = rr64::netplay::get_status();
        if (updated.phase==rr64::netplay::Phase::Race) {
            if(race_started==std::chrono::steady_clock::time_point{}) race_started=std::chrono::steady_clock::now();
            if(race_seconds) ++motion_frame;
            rr64::netplay::RiderState rider{};
            rider.active=true; rider.tick=1;rider.sample_time_us=1000000+updated.local_slot+motion_frame*10000ULL;
            if(race_seconds) rider.sample_time_us=std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
            rider.position_x=100.f+updated.local_slot+motion_frame;
            rider.root.rider_anchor={70.f+updated.local_slot,80.f,90.f};
            rider.root.drive_lockout=updated.local_slot%2;
            rider.root.bike_origin={10.f+updated.local_slot,20,30};rider.root.rider_velocity={40,50.f+updated.local_slot,60};rider.root.valid=1;rider.root.bike_height=300.f+updated.local_slot;rider.root.bike_rotation={0,0,0,1};rider.root.rider_rotation={0,0,1,0};
            rider.rider_position_valid=1; rider.rider_x=200.f+updated.local_slot+motion_frame;
            rider.front_wheel_x=rider.position_x+1;
            rider.rear_wheel_x=rider.position_x-1;
            rider.weapon=5;rider.root.equipment_valid=1;rider.root.inventory[5]=3;
            rr64::world_sync::Snapshot world{};world.round=updated.game_setup.revision;world.tick=1;
            world.traffic[0].active=1;world.traffic[0].id=77;world.traffic[0].kind=1;world.traffic[0].model=0xD8;world.traffic[0].position[0]=123;
            rr64::world_sync::Snapshot prior{};if(rr64::netplay::get_world_state(prior))world.tick=prior.tick+1;
            if(rr64::netplay::set_host_world_state(world)!=host) return 22;
            rider.root.durability=87.25f+updated.local_slot; rider.root.durability_capacity=150.5f;
            rr64::netplay::set_local_rider_state(rider);
            // Clients cannot author AI; host cannot overwrite a human slot.
            if(rr64::netplay::set_host_ai_rider_state(updated.local_slot,rider)) return 20;
            if(expected_players<14) {
                auto ai=rider;ai.position_x=999;ai.character=3;ai.bike=2;
                const bool accepted=rr64::netplay::set_host_ai_rider_state(13,ai);
                if(accepted!=host) return 21;
            }
            rr64::netplay::RiderState before_invalid{};rr64::netplay::get_rider_state(updated.local_slot,before_invalid);
            auto invalid=rider;invalid.tick=before_invalid.tick+1;
            invalid.root.bike_origin[0]=std::numeric_limits<float>::quiet_NaN();
            rr64::netplay::set_local_rider_state(invalid);
            rr64::netplay::RiderState after_invalid{};rr64::netplay::get_rider_state(updated.local_slot,after_invalid);
            if(before_invalid.tick!=after_invalid.tick) return 6;

        }
        const auto now = std::chrono::steady_clock::now();
        if (updated.phase == rr64::netplay::Phase::Race &&
            updated.local_slot < rr64::netplay::kMaximumPlayers &&
            now - last_voice_send >= std::chrono::milliseconds(50)) {
            const std::uint8_t payload[] = {
                0x56u,
                updated.local_slot,
                static_cast<std::uint8_t>(0xA0u + updated.local_slot),
            };
            rr64::netplay::submit_local_voice(payload);
            last_voice_send = now;
        }
        for (const rr64::netplay::ReceivedVoiceFrame& voice :
             rr64::netplay::take_received_voice_frames()) {
            remote_voice_received = remote_voice_received ||
                (voice.speaker_slot != updated.local_slot && voice.payload_size == 3 &&
                 voice.payload[0] == 0x56u && voice.payload[1] == voice.speaker_slot);
        }
        bool inputs_synchronized = true;
        for (std::uint8_t slot = 0; slot < rr64::netplay::kMaximumPlayers; ++slot) {
            if (!updated.players[slot].connected) {
                continue;
            }
            std::uint16_t buttons = 0;
            float stick_x = 0.0f;
            float stick_y = 0.0f;
            rr64::netplay::RiderState rider{};
            const bool received=rr64::netplay::get_rider_state(slot,rider);
            if(received && rider.tick!=observed_tick[slot]) {
                if(observed_tick[slot] && rider.tick<observed_tick[slot]) return 23;
                if(observed_updates[slot]) maximum_gap_ms[slot]=std::max(maximum_gap_ms[slot],
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(now-observed_at[slot]).count()));
                if(observed_updates[slot]) gaps_us[slot].push_back(std::chrono::duration_cast<std::chrono::microseconds>(now-observed_at[slot]).count());
                if(race_seconds && slot!=updated.local_slot) {
                    const auto age=std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count()-static_cast<long long>(rider.sample_time_us);
                    if(age<0 || age>5000000) return 25;
                    ages_us[slot].push_back(age);
                }
                observed_at[slot]=now;observed_tick[slot]=rider.tick;++observed_updates[slot];
            }
            const auto received_frame=race_seconds && received ? unsigned(rider.position_x-100.f-slot) : 0;
            inputs_synchronized = inputs_synchronized &&
                rr64::netplay::get_rider_state(slot,rider) && rider.active &&

            (race_seconds ? rider.sample_time_us>0 : rider.sample_time_us==1000000+slot) && rider.weapon==5 && rider.root.equipment_valid && rider.root.inventory[5]==3 && rider.root.durability==87.25f+slot && rider.root.durability_capacity==150.5f && rider.root.rider_anchor[0]==70.f+slot && rider.root.rider_anchor[2]==90.f && rider.root.drive_lockout==slot%2 && rider.root.bike_origin[0]==10.f+slot && rider.root.rider_velocity[1]==50.f+slot && rider.root.valid==1 && rider.root.bike_height==300.f+slot && rider.root.bike_rotation[3]==1 && rider.root.rider_rotation[2]==1 && rider.position_x==100.f+slot+received_frame && rider.rider_position_valid==1 && rider.rider_x==200.f+slot+received_frame &&
                rider.front_wheel_x==rider.position_x+1 && rider.rear_wheel_x==rider.position_x-1 &&
                rr64::netplay::get_player_input(slot, buttons, stick_x, stick_y) &&
                buttons == static_cast<std::uint16_t>((slot==0 ? 0x5000u : 0x4000u) | slot) &&
                updated.players[slot].selection.rider==unsigned(slot)+1 &&
                updated.players[slot].selection.loaded==1;
        }
        if(expected_players<14 && updated.phase==rr64::netplay::Phase::Race) {
            rr64::netplay::RiderState ai{};
            inputs_synchronized=inputs_synchronized && rr64::netplay::get_rider_state(13,ai) &&
                ai.host_ai && ai.position_x==999 && ai.character==3 && ai.bike==2;
        }
        rr64::world_sync::Snapshot world{};
        inputs_synchronized=inputs_synchronized && rr64::netplay::get_world_state(world) &&
            world.round==updated.game_setup.revision && world.traffic[0].id==77 && world.traffic[0].position[0]==123;
        passed = updated.connected && updated.connected_players >= expected_players &&
            updated.local_slot < rr64::netplay::kMaximumPlayers &&
            updated.phase == rr64::netplay::Phase::Race &&
            updated.game_setup.valid && updated.game_setup.revision > 0 &&
            updated.game_setup.transition_buttons == 0x8000u &&
            updated.game_setup.race_options == (512u | (7u<<6) | (3u<<4) | 6u) &&
            updated.game_setup.random_seed == 0x12345678 &&
            updated.game_setup.start_requested == 1 &&
            updated.game_setup.words.front() == 0x64000000u &&
            updated.game_setup.words.back() ==
                0x64000000u + static_cast<std::uint32_t>(updated.game_setup.words.size() - 1) &&
            inputs_synchronized && remote_voice_received;
        if (passed && (!race_seconds || now-race_started>=std::chrono::seconds(race_seconds))) {
            if(race_seconds) for(unsigned slot=0;slot<14;++slot)
                if(updated.players[slot].connected && observed_updates[slot]<unsigned(race_seconds*5)) passed=false;
            if(!passed) return 24;
            break;
        }
        passed=false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    if (passed) {
        const auto grace_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (std::chrono::steady_clock::now() < grace_deadline) {
            rr64::netplay::update();
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    if (!passed) {
        const auto s=rr64::netplay::get_status();
        std::fprintf(stderr,"phase=%u peers=%u slot=%u voice=%u\n",unsigned(s.phase),unsigned(s.connected_players),unsigned(s.local_slot),unsigned(remote_voice_received));
        for (unsigned slot=0;slot<14;++slot) if (s.players[slot].connected) {
            const auto &p=s.players[slot]; rr64::netplay::RiderState r{};
            const bool got=rr64::netplay::get_rider_state(slot,r);
            std::fprintf(stderr,"slot=%u ready=%u confirmed=%u loaded=%u state=%u x=%f\n",slot,unsigned(p.ready),p.selection.confirmed,p.selection.loaded,unsigned(got),r.position_x);
        }
    }
    for(unsigned slot=0;slot<14;++slot) if(observed_updates[slot])
        std::fprintf(stderr,"[RR64-NET-MOTION] slot=%u updates=%u max_gap_ms=%lld\n",slot,observed_updates[slot],maximum_gap_ms[slot]);
    for(unsigned slot=0;slot<14;++slot) if(!ages_us[slot].empty()) {
        auto &ages=ages_us[slot];auto &gaps=gaps_us[slot];std::sort(ages.begin(),ages.end());std::sort(gaps.begin(),gaps.end());
        std::fprintf(stderr,"[RR64-NET-DELAY] slot=%u age_p50_us=%lld age_p95_us=%lld gap_p95_us=%lld\n",slot,
            ages[ages.size()/2],ages[(ages.size()-1)*95/100],gaps.empty()?0:gaps[(gaps.size()-1)*95/100]);
    }
    rr64::netplay::shutdown();
    std::fprintf(stderr, "[RR64-NET-TEST] %s %s.\n", mode.c_str(), passed ? "passed" : "failed");
    return passed ? 0 : 1;
}

