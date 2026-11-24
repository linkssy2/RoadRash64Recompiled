#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
#include "rr64_prediction_diagnostics.hpp"
#include "rr64_process_exit.hpp"

namespace rr64::sync_log {
inline bool campaign() {
    static const bool enabled = [] { const char* p=std::getenv("RR64_CAMPAIGN_LOG"); return p && *p=='1'; }();
    return enabled;
}
struct Rider {
    std::array<float,3> bike_position{},body_position{},body_velocity{};
    unsigned bike_attached=0,body_attached=0,ejected=0;
    unsigned host_ai=0, weapon=0, received_weapon=0;
    float durability=0, received_durability=0;
    unsigned presentation_matrices=0;
    std::array<float,3> presentation_delta{};
    unsigned actor_bike = 0;
    unsigned root_hash = 0, received_root_hash = 0;
    unsigned valid = 0, input_valid = 0, buttons = 0, received = 0, tick = 0;
    float x = 0, y = 0, z = 0, stick_x = 0, stick_y = 0;
    float received_x = 0, received_y = 0, received_z = 0;
};
struct Sample {
    prediction::ReconcileDiagnostic reconciliation{};
    unsigned traffic_valid=0,traffic_count=0,traffic_hash=0;
    unsigned host = 0, local = 0, race = 0, frame = 0, phase = 0;
    unsigned setup = 0, options = 0, rng = 0, stage = 0;
    unsigned long long us = 0;
    std::array<Rider, 14> riders{};
};

// Single game-thread producer, single file-writing consumer. No file writes,
// allocations or waits in submit(). Overflow is recorded, never stalls play.
class Writer {
    static constexpr unsigned capacity = 256;
    std::array<Sample, capacity> queue{};
    std::atomic<unsigned> head{0}, tail{0};
    std::atomic<unsigned long long> dropped{0};
    std::atomic<bool> stopped{false};
    std::thread worker;
    void run(std::filesystem::path path) {
        auto *file = std::fopen(path.string().c_str(), "wx");
        if (!file) {
            std::fprintf(stderr, "[RR64-SYNC] Cannot create log; capture unavailable.\n");
            stopped.store(true);
            return;
        }
        std::fprintf(file, "host,local,race,frame,phase,setup,options,rng,stage,us,slot,valid,input_valid,buttons,stick_x,stick_y,x,y,z,received,tick,received_x,received_y,received_z,dropped,root_hash,received_root_hash,actor_bike,presentation_matrices,presentation_dx,presentation_dy,presentation_dz,host_ai,weapon,received_weapon,durability,received_durability,bike_x,bike_y,bike_z,body_x,body_y,body_z,body_vx,body_vy,body_vz,bike_attached,body_attached,ejected,traffic_valid,traffic_count,traffic_hash,reconcile_sequence,reconcile_round,reconcile_tick,reconcile_ack,reconcile_pending,reconcile_steps,reconcile_stage,reconcile_us,reconcile_distance,reconcile_simulated_us,reconcile_history_us,reconcile_replay_us,camera_view,camera_before_valid,camera_before_mode,camera_before_actor,camera_before_flags,camera_before_qx,camera_before_qy,camera_before_qz,camera_before_qw,camera_before_anchor_x,camera_before_anchor_y,camera_before_anchor_z,camera_replayed_valid,camera_replayed_mode,camera_replayed_actor,camera_replayed_flags,camera_replayed_qx,camera_replayed_qy,camera_replayed_qz,camera_replayed_qw,camera_replayed_anchor_x,camera_replayed_anchor_y,camera_replayed_anchor_z,camera_after_valid,camera_after_mode,camera_after_actor,camera_after_flags,camera_after_qx,camera_after_qy,camera_after_qz,camera_after_qw,camera_after_anchor_x,camera_after_anchor_y,camera_after_anchor_z,camera_authority_valid,camera_authority_flags,camera_authority_qx,camera_authority_qy,camera_authority_qz,camera_authority_qw\n");
        unsigned long long rows = 0;
        while (true) {
            auto read = tail.load(std::memory_order_relaxed);
            const auto write = head.load(std::memory_order_acquire);
            while (read != write) {
                const auto &s = queue[read % capacity];
                for (unsigned slot = 0; slot < s.riders.size(); ++slot) {
                    const auto &r = s.riders[slot];
                    if (!r.valid && !r.received) continue;
                    std::fprintf(file, "%u,%u,%u,%u,%u,%u,%u,%u,%u,%llu,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%.9g,%.9g,%.9g,%llu,%u,%u,%u,%u,%.9g,%.9g,%.9g,%u,%u,%u,%.9g,%.9g",
                        s.host,s.local,s.race,s.frame,s.phase,s.setup,s.options,s.rng,s.stage,s.us,
                        slot,r.valid,r.input_valid,r.buttons,r.stick_x,r.stick_y,r.x,r.y,r.z,
                        r.received,r.tick,r.received_x,r.received_y,r.received_z,dropped.load(),r.root_hash,r.received_root_hash,r.actor_bike,r.presentation_matrices,r.presentation_delta[0],r.presentation_delta[1],r.presentation_delta[2],r.host_ai,r.weapon,r.received_weapon,r.durability,r.received_durability);
                    std::fprintf(file, ",%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u,%u,%u,%u,%u", r.bike_position[0],r.bike_position[1],r.bike_position[2],r.body_position[0],r.body_position[1],r.body_position[2],r.body_velocity[0],r.body_velocity[1],r.body_velocity[2],r.bike_attached,r.body_attached,r.ejected,s.traffic_valid,s.traffic_count,s.traffic_hash);
                    const auto &d=s.reconciliation;
                    std::fprintf(file, ",%u,%u,%u,%u,%u,%u,%u,%llu,%.9g,%llu,%llu,%llu",d.sequence,d.round,d.tick,d.ack,d.pending,d.steps,d.stage,static_cast<unsigned long long>(d.us),d.distance,
                        static_cast<unsigned long long>(d.simulated_us),static_cast<unsigned long long>(d.history_us),static_cast<unsigned long long>(d.replay_us));
                    std::fprintf(file, ",%u",d.camera_view);
                    for(const auto *c:{&d.camera_before,&d.camera_replayed,&d.camera_after})
                        std::fprintf(file, ",%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g",
                            c->valid,c->mode,c->mapped_actor,c->state_flags,
                            c->rotation[0],c->rotation[1],c->rotation[2],c->rotation[3],
                            c->anchor[0],c->anchor[1],c->anchor[2]);
                    const auto &a=d.camera_authority;
                    std::fprintf(file, ",%u,%u,%.9g,%.9g,%.9g,%.9g\n",a.valid,a.state_flags,
                        a.rotation[0],a.rotation[1],a.rotation[2],a.rotation[3]);
                    ++rows;
                }
                tail.store(++read, std::memory_order_release);
            }
            std::fflush(file); // periodic durability; a crash can lose the queued tail
            if (std::ferror(file) || rows >= (campaign() ? 10000000ull : 250000ull)) {
                std::fprintf(file, "# incomplete: write error or row limit\n");
                stopped.store(true);
                break;
            }
            if (stopped.load() && read == head.load()) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        std::fprintf(file, "# end dropped=%llu\n", dropped.load());
        std::fclose(file);
    }
public:
    explicit Writer(const std::filesystem::path &path) : worker([this, path]{run(path);}) {}
    ~Writer() { stop(); }
    void stop() { stopped.store(true); if (worker.joinable()) worker.join(); }
    void submit(const Sample &sample) {
        if (stopped.load()) return;
        const auto write = head.load(std::memory_order_relaxed);
        if (write - tail.load(std::memory_order_acquire) >= capacity) { ++dropped; return; }
        queue[write % capacity] = sample;
        head.store(write + 1, std::memory_order_release);
    }
};

inline Writer *writer = nullptr;
inline void shutdown() { if (writer) writer->stop(); }
// Called once at process startup, before game threads exist. The caller selects
// a unique filename; exclusive creation prevents overwriting historical logs.
inline void initialize() {
    const char *path = std::getenv("RR64_SYNC_LOG");
    if (path && *path) {
        try {
            writer = new Writer(std::filesystem::path(path));
            std::atexit(shutdown);
            rr64::process::at_quick_exit(shutdown);
        } catch (...) {
            std::fprintf(stderr, "[RR64-SYNC] Unable to start capture worker.\n");
        }
    }
}
}
