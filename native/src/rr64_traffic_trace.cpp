#include "rr64_msvc_crt_compat.hpp"
#include "rr64_engine_layout.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>

namespace {
struct Sample {
    unsigned long long ns = 0;
    unsigned kind = 0, entity = 0, view = 0, result = 0, a = 0, b = 0;
    float x = 0, y = 0, z = 0;
};
struct Key {
    unsigned entity = 0, kind = 0, view = 0, result = 0;
};
class Trace {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<Sample, 256> queue{};
    std::array<Key, 512> keys{};
    unsigned count = 0, nextKey = 0;
    bool stopping = false;
    std::atomic<unsigned> lost{0};
    std::thread worker;

  public:
    Trace() : worker([this] { drain(); }) {}
    ~Trace() {
        {
            std::lock_guard lock(mutex);
            stopping = true;
        }
        changed.notify_one();
        worker.join();
    }
    void add(unsigned char *m, unsigned kind, unsigned entity, unsigned result, unsigned a,
             unsigned b) {
        using namespace rr64::engine;
        if (!valid_guest_range(entity, traffic_scene::entity_minimum_size))
            return;
        unsigned view = 0;
        read_u32(m, globals::active_viewport, view);
        std::unique_lock lock(mutex, std::try_to_lock);
        if (!lock.owns_lock()) {
            ++lost;
            return;
        }
        // Lifecycle events always survive deduplication. Visibility decisions
        // record transitions, not every car on every frame.
        if (kind >= 2) {
            Key *key = nullptr;
            for (auto &k : keys)
                if (k.entity == entity && k.kind == kind && k.view == view) {
                    key = &k;
                    break;
                }
            if (key && key->result == result)
                return;
            if (!key)
                key = &keys[nextKey++ % keys.size()];
            *key = {entity, kind, view, result};
        } else {
            for (auto &k : keys)
                if (k.entity == entity)
                    k = {};
        }
        if (count == queue.size()) {
            ++lost;
            return;
        }
        Sample s;
        s.ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
                   .count();
        s.kind = kind;
        s.entity = entity;
        s.view = view;
        s.result = result;
        s.a = a;
        s.b = b;
        read_float(m, entity + 0xA8, s.x);
        read_float(m, entity + 0xAC, s.y);
        read_float(m, entity + 0xB0, s.z);
        queue[count++] = s;
    }
    void drain() {
        FILE *f = nullptr;
        fopen_s(&f, "traffic-events.log", "w");
        if (f)
            std::fputs(
                "# kinds: 0=spawn-before-placement(position not final) 1=recycle 2=range(1=visible) 3=frustum(1=visible) 4=draw(0=visible); a/b are raw argument bits; timestamps monotonic ns\n",
                f);
        std::array<Sample, 256> batch{};
        unsigned total = 0;
        for (;;) {
            unsigned n = 0;
            bool done = false;
            {
                std::unique_lock lock(mutex);
                changed.wait_for(lock, std::chrono::seconds(1), [&] { return stopping; });
                n = count;
                std::copy_n(queue.begin(), n, batch.begin());
                count = 0;
                done = stopping;
            }
            const unsigned drops = lost.exchange(0);
            if (f && total < 32768) {
                if (drops)
                    std::fprintf(f, "lost=%u\n", drops);
                for (unsigned i = 0; i < n && total < 32768; ++i, ++total) {
                    const auto &s = batch[i];
                    std::fprintf(
                        f,
                        "ns=%llu kind=%u entity=%08X view=%u result=%u a=%08X b=%08X pos=%.3f,%.3f,%.3f\n",
                        s.ns, s.kind, s.entity, s.view, s.result, s.a, s.b, s.x, s.y, s.z);
                }
                if (total == 32768)
                    std::fputs("capture-limit-reached\n", f);
                std::fflush(f);
            }
            if (done)
                break;
        }
        if (f) {
            std::fputs("capture-ended\n", f);
            std::fclose(f);
        }
    }
};
}
extern "C" void rr64_traffic_trace(unsigned char *m, unsigned kind, unsigned address,
                                   unsigned result, unsigned a, unsigned b) {
    static const bool enabled = [] {
        char *value = nullptr;
        size_t n = 0;
        _dupenv_s(&value, &n, "RR64_TRAFFIC_DIAGNOSTICS");
        bool on = value && value[0] == '1';
        std::free(value);
        return on;
    }();
    if (!enabled || !m)
        return;
    using namespace rr64::engine;
    unsigned entity = address, type = 0;
    if (kind >= 2 && (!read_u32(m, address, type) || type != traffic_scene::node_type ||
                      !read_u32(m, address + actor_scene::entity, entity)))
        return;
    static Trace trace;
    trace.add(m, kind, entity, result, a, b);
}
