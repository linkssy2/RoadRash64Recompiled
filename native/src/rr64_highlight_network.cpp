#include "rr64_highlight_network.hpp"
#include <zstd.h>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>

namespace rr64::highlight_network {
namespace {
static_assert(sizeof(unsigned) == 4);
constexpr std::uint32_t format_magic = 0x344c4852; // RHL4: recorded MK64 item/effect presentation.
constexpr std::uint64_t timeout_us = 60000000, resend_us = 250000;
constexpr unsigned window = 64, all_peers = (1u << 14) - 1;
std::uint64_t hash(std::span<const std::uint8_t> data) {
    std::uint64_t h = 14695981039346656037ull;
    for (auto b : data) {
        h ^= b;
        h *= 1099511628211ull;
    }
    return h;
}
struct Writer {
    std::vector<std::uint8_t> bytes;
    template <class T> void value(T v) {
        if constexpr (std::is_same_v<T, bool>)
            value<std::uint8_t>(v ? 1 : 0);
        else if constexpr (std::is_same_v<T, float>)
            value(std::bit_cast<std::uint32_t>(v));
        else {
            if (bytes.size() > maximum_raw_bytes - sizeof(T))
                throw std::runtime_error("size");
            for (unsigned i = 0; i < sizeof(T); ++i)
                bytes.push_back(std::uint8_t(std::uint64_t(v) >> (i * 8)));
        }
    }
    template <class T, std::size_t N> void array(const std::array<T, N> &a) {
        for (auto v : a)
            value(v);
    }
};
struct Reader {
    std::span<const std::uint8_t> bytes;
    std::size_t at = 0;
    template <class T> void value(T &v) {
        if constexpr (std::is_same_v<T, bool>) {
            std::uint8_t b;
            value(b);
            if (b > 1)
                throw std::runtime_error("boolean");
            v = b != 0;
        } else if constexpr (std::is_same_v<T, float>) {
            std::uint32_t u;
            value(u);
            v = std::bit_cast<float>(u);
        } else {
            if (at > bytes.size() || bytes.size() - at < sizeof(T))
                throw std::runtime_error("truncated");
            std::uint64_t x = 0;
            for (unsigned i = 0; i < sizeof(T); ++i)
                x |= std::uint64_t(bytes[at++]) << (i * 8);
            v = static_cast<T>(x);
        }
    }
    template <class T, std::size_t N> void array(std::array<T, N> &a) {
        for (auto &v : a)
            value(v);
    }
};
// Decode directly into owned frames. Holding a second uncompressed playlist
// would otherwise double peak memory for a full fourteen-racer recording.
struct StreamReader {
    std::unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> stream{ZSTD_createDStream(),
                                                                      ZSTD_freeDStream};
    ZSTD_inBuffer input{};
    std::array<std::uint8_t, 65536> buffer{};
    std::size_t offset = 0, size = 0, at = 0, expected = 0;
    bool finished = false;
    explicit StreamReader(const Blob &blob)
        : input{blob.bytes.data(), blob.bytes.size(), 0}, expected(blob.raw_size) {
        if (!stream || ZSTD_isError(ZSTD_initDStream(stream.get())) ||
            ZSTD_isError(ZSTD_DCtx_setParameter(stream.get(), ZSTD_d_windowLogMax, 23)))
            throw std::runtime_error("zstd");
    }
    std::uint8_t byte() {
        if (offset == size) {
            if (finished)
                throw std::runtime_error("truncated");
            ZSTD_outBuffer output{buffer.data(), buffer.size(), 0};
            auto status = ZSTD_decompressStream(stream.get(), &output, &input);
            if (ZSTD_isError(status) || !output.pos)
                throw std::runtime_error("zstd");
            finished = status == 0;
            offset = 0;
            size = output.pos;
        }
        if (++at > expected)
            throw std::runtime_error("size");
        return buffer[offset++];
    }
    template <class T> void value(T &v) {
        if constexpr (std::is_same_v<T, bool>) {
            auto b = byte();
            if (b > 1)
                throw std::runtime_error("boolean");
            v = b != 0;
        } else if constexpr (std::is_same_v<T, float>) {
            std::uint32_t u;
            value(u);
            v = std::bit_cast<float>(u);
        } else {
            std::uint64_t x = 0;
            for (unsigned i = 0; i < sizeof(T); ++i)
                x |= std::uint64_t(byte()) << (i * 8);
            v = static_cast<T>(x);
        }
    }
    template <class T, std::size_t N> void array(std::array<T, N> &a) {
        for (auto &v : a)
            value(v);
    }
    bool complete() const {
        return finished && offset == size && input.pos == input.size && at == expected;
    }
};
template <class IO> void pose(IO &io, highlights::Pose &p) {
    io.value(p.valid);
    if (!p.valid)
        return;
    io.value(p.count);
    io.value(p.record_count);
    io.value(p.lod);
    io.value(p.source_bank);
    io.value(p.topology);
    if (p.count > highlights::maximum_bones)
        throw std::runtime_error("bones");
    for (unsigned i = 0; i < p.count; ++i) {
        auto &b = p.bones[i];
        io.value(b.index);
        io.value(b.type);
        io.array(b.values);
    }
}
template <class IO> void weapon(IO &io, highlights::WeaponPose &p) {
    io.value(p.valid);
    if (!p.valid)
        return;
    io.value(p.model);
    io.value(p.count);
    io.value(p.topology);
    if (!p.count || p.count > highlights::maximum_weapon_records)
        throw std::runtime_error("weapon records");
    for (unsigned i = 0; i < p.count; ++i)
        io.array(p.transforms[i]);
}
template <class IO> void item_state(IO &io, mk64_items::Snapshot &s) {
    io.value(s.enabled);
    if (!s.enabled)
        return;
    if (s.enabled != 1)
        throw std::runtime_error("item mode");
    io.value(s.clock);
    io.value(s.next_generation);
    io.value(s.random);
    for (auto &r : s.riders) {
        io.value(r.held); io.value(r.charges); io.value(r.deployed); io.value(r.revision);
        io.value(r.star_until); io.value(r.boo_until); io.value(r.shrink_until);
        io.value(r.boost_until); io.value(r.golden_until); io.value(r.last_use);
        io.value(r.hit_until); io.value(r.boost_speed); io.value(r.event_serial);
        io.value(r.cue); io.value(r.event_target);
    }
    for (auto &o : s.objects) {
        io.value(o.generation);
        if (!o.generation)
            continue;
        io.value(o.born); io.value(o.expires); io.value(o.kind); io.value(o.mode);
        io.value(o.owner); io.value(o.target); io.value(o.orbit); io.value(o.bounces);
        io.array(o.position); io.array(o.velocity);
    }
    if (!mk64_items::valid(s))
        throw std::runtime_error("item state");
}
template <class IO> void frame(IO &io, highlights::Frame &f) {
    io.value(f.time_us);
    io.value(f.tick);
    for (auto &r : f.racers) {
        io.value(r.active);
        if (!r.active)
            continue;
        io.value(r.model);
        io.value(r.character);
        io.value(r.weapon);
        io.value(r.crash_flags);
        io.value(r.generation);
        io.array(r.bike_origin);
        io.array(r.rider_origin);
        io.array(r.bike_anchor);
        io.array(r.rider_anchor);
        io.array(r.bike_rotation);
        io.array(r.rider_rotation);
        pose(io, r.bike_pose);
        pose(io, r.rider_pose);
        weapon(io, r.held_weapon);
    }
    // Offline replays have extra slots; the existing online format remains 20.
    for (unsigned i = 0; i < world_sync::capacity; ++i) {
        auto &t = f.traffic[i];
        io.value(t.active);
        if (!t.active)
            continue;
        io.value(t.id);
        io.value(t.model);
        io.value(t.kind);
        io.array(t.position);
        io.array(t.velocity);
        io.array(t.angles);
        io.value(t.motion_valid);
        io.value(t.road_distance);
        io.array(t.motion);
        io.array(t.directions);
    }
    auto &h = f.hazards;
    io.value(h.clock);
    io.value(h.count);
    if (h.count > netplay::kMaximumCourseHazards)
        throw std::runtime_error("hazards");
    for (unsigned i = 0; i < h.count; ++i) {
        auto &p = h.poses[i];
        io.array(p.position);
        io.array(p.velocity);
        io.array(p.rotation);
        io.value(p.model);
        io.value(p.generation);
        io.value(p.active);
        io.value(p.visual_scale);
        io.value(p.opacity);
        io.value(p.tint);
        io.value(p.environment_tint);
    }
    item_state(io, f.items);
}
bool valid_clip(const highlights::Clip &c) {
    if (c.slot >= highlights::maximum_racers || !std::isfinite(c.score) || c.score <= 0 ||
        c.score > 1e9f || c.frames.empty() || c.frames.size() > highlights::maximum_clip_frames ||
        c.event_tick < c.frames.front().tick || c.event_tick > c.frames.back().tick ||
        c.event_time_us < c.frames.front().time_us || c.event_time_us > c.frames.back().time_us ||
        c.frames.back().time_us - c.frames.front().time_us >
            highlights::before_us + highlights::after_us + 100000)
        return false;
    for (unsigned i = 0; i < c.frames.size(); ++i)
        if (!highlights::valid_frame(c.frames[i]) ||
            (i && (c.frames[i].tick <= c.frames[i - 1].tick ||
                   c.frames[i].time_us <= c.frames[i - 1].time_us)))
            return false;
    return true;
}
template <class IO> void clip_header(IO &io, highlights::Clip &c, std::uint32_t &count) {
    io.value(c.slot);
    io.value(c.score);
    io.value(c.event_tick);
    io.value(c.event_time_us);
    io.value(c.truncated_before);
    io.value(c.truncated_after);
    io.value(c.capacity_limited);
    io.value(count);
}
enum class Message : std::uint8_t { Manifest = 1, Chunk, Ack, Control };
Writer message(Message type, unsigned round) {
    Writer w;
    w.value(type);
    w.value(round);
    return w;
}
std::uint64_t elapsed(std::uint64_t now, std::uint64_t start) {
    return now >= start ? now - start : 0;
}
}

bool encode(std::span<const highlights::Clip> clips, Blob &out) noexcept {
    try {
        if (clips.empty() || clips.size() > highlights::maximum_clips)
            return false;
        Writer w;
        std::size_t reserve = 8;
        for (const auto &c : clips) {
            if (!valid_clip(c))
                return false;
            for (const auto &f : c.frames)
                for (unsigned i = world_sync::capacity; i < f.traffic.size(); ++i)
                    if (f.traffic[i].active) return false;
            reserve += 64 + c.frames.size() * sizeof(highlights::Frame);
        }
        if (reserve > maximum_raw_bytes)
            return false;
        w.bytes.reserve(reserve);
        w.value(format_magic);
        w.value(std::uint32_t(clips.size()));
        for (auto c : clips) {
            if (!valid_clip(c))
                return false;
            auto count = std::uint32_t(c.frames.size());
            clip_header(w, c, count);
            for (const auto &f : c.frames)
                frame(w, const_cast<highlights::Frame &>(f));
        }
        Blob next;
        next.raw_size = std::uint32_t(w.bytes.size());
        next.bytes.resize(
            std::min<std::size_t>(ZSTD_compressBound(w.bytes.size()), maximum_compressed_bytes));
        auto size =
            ZSTD_compress(next.bytes.data(), next.bytes.size(), w.bytes.data(), w.bytes.size(), 3);
        if (ZSTD_isError(size) || !size || size > maximum_compressed_bytes)
            return false;
        next.bytes.resize(size);
        next.checksum = hash(next.bytes);
        out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}
bool decode(const Blob &blob, Playlist &out) noexcept {
    try {
        if (blob.bytes.empty() || blob.bytes.size() > maximum_compressed_bytes || !blob.raw_size ||
            blob.raw_size > maximum_raw_bytes || hash(blob.bytes) != blob.checksum)
            return false;
        if (ZSTD_getFrameContentSize(blob.bytes.data(), blob.bytes.size()) != blob.raw_size ||
            ZSTD_findFrameCompressedSize(blob.bytes.data(), blob.bytes.size()) != blob.bytes.size())
            return false;
        StreamReader r{blob};
        unsigned magic = 0, count = 0;
        r.value(magic);
        r.value(count);
        if (magic != format_magic || !count || count > highlights::maximum_clips)
            return false;
        Playlist next;
        next.count = count;
        for (unsigned i = 0; i < count; ++i) {
            auto &c = next.clips[i];
            std::uint32_t n = 0;
            clip_header(r, c, n);
            if (!n || n > highlights::maximum_clip_frames)
                return false;
            next.frames[i].resize(n);
            for (auto &f : next.frames[i])
                frame(r, f);
            c.frames = next.frames[i];
            if (!valid_clip(c))
                return false;
        }
        if (!r.complete())
            return false;
        out = std::move(next);
        return true;
    } catch (...) {
        return false;
    }
}

struct Channel::State {
    bool host = false, taken = false, ready = false, published = false;
    unsigned round = 0, count = 0, base = 0, compressed_size = 0, control_serial = 0;
    Stage stage = Stage::Idle;
    Blob blob;
    std::uint64_t duration = 0, started = 0, first = 0, control = 0, ack = 0, service = 0,
                  credit = 0;
    std::vector<bool> received;
    struct Peer {
        unsigned base = 0;
        std::uint64_t mask = 0;
        bool ready = false;
        std::array<std::uint64_t, window> sent{};
        std::array<unsigned, window> part{};
        Peer() {
            part.fill(~0u);
        }
    };
    std::array<Peer, 14> peers;
};
Channel::Channel() = default;
Channel::~Channel() = default;
Channel::Channel(Channel &&) noexcept = default;
Channel &Channel::operator=(Channel &&) noexcept = default;
void Channel::reset(bool host, unsigned round) {
    state_ = std::make_unique<State>();
    state_->host = host;
    state_->round = round;
}
bool Channel::publish(Blob blob, std::uint64_t duration, std::uint64_t now) {
    if (!state_ || !state_->host || !state_->round || state_->published ||
        state_->stage == Stage::Finished)
        return false;
    auto &s = *state_;
    s.published = true;
    s.first = now;
    if (blob.bytes.empty()) {
        s.stage = Stage::Finished;
        return true;
    }
    if (blob.bytes.size() > maximum_compressed_bytes || !blob.raw_size ||
        blob.raw_size > maximum_raw_bytes || hash(blob.bytes) != blob.checksum || !duration ||
        duration > 60000000) {
        s.stage = Stage::Finished;
        return false;
    }
    s.blob = std::move(blob);
    s.count = unsigned((s.blob.bytes.size() + chunk_bytes - 1) / chunk_bytes);
    s.duration = duration;
    s.stage = Stage::Transferring;
    return true;
}
void Channel::receive(unsigned peer, std::span<const std::uint8_t> bytes, std::uint64_t now,
                      std::uint64_t one_way) {
    try {
        if (!state_ || bytes.size() > maximum_datagram || !state_->round ||
            state_->stage == Stage::Finished)
            return;
        auto &s = *state_;
        Reader r{bytes};
        Message type;
        unsigned round;
        r.value(type);
        r.value(round);
        if (round != s.round)
            return;
        if (s.host) {
            if (type != Message::Ack || !peer || peer >= 14 || !s.published)
                return;
            unsigned base;
            std::uint64_t mask;
            bool ready;
            r.value(base);
            r.value(mask);
            r.value(ready);
            auto &p = s.peers[peer];
            if (r.at != bytes.size() || base > s.count || base < p.base || base > p.base + window ||
                (ready && base != s.count) || (mask & 1) ||
                (s.count - base < window && (mask >> (s.count - base))))
                return;
            for (unsigned part = p.base; part < base; ++part)
                if (p.part[part % window] != part)
                    return;
            for (unsigned i = 0; i < window && base + i < s.count; ++i)
                if ((mask & (1ull << i)) && p.part[(base + i) % window] != base + i)
                    return;
            p.base = base;
            p.mask = mask;
            p.ready |= ready;
            return;
        }
        if (peer != 0)
            return;
        if (type == Message::Manifest) {
            unsigned size, raw, count;
            std::uint64_t checksum, duration;
            r.value(size);
            r.value(raw);
            r.value(count);
            r.value(checksum);
            r.value(duration);
            if (r.at != bytes.size() || !size || size > maximum_compressed_bytes || !raw ||
                raw > maximum_raw_bytes || count != (size + chunk_bytes - 1) / chunk_bytes ||
                !duration || duration > 60000000)
                return;
            if (s.published) {
                if (s.compressed_size != size || s.count != count || s.blob.raw_size != raw ||
                    s.blob.checksum != checksum || s.duration != duration)
                    s.stage = Stage::Finished;
                return;
            }
            s.blob.bytes.resize(size);
            s.blob.raw_size = raw;
            s.blob.checksum = checksum;
            s.count = count;
            s.duration = duration;
            s.compressed_size = size;
            s.received.assign(count, false);
            s.published = true;
            s.stage = Stage::Transferring;
            if (!s.first)
                s.first = now;
        } else if (type == Message::Chunk) {
            unsigned part;
            r.value(part);
            if (!s.published || s.taken || part >= s.count || part >= s.base + window)
                return;
            const auto offset = std::size_t(part) * chunk_bytes,
                       n = std::min<std::size_t>(chunk_bytes, s.blob.bytes.size() - offset);
            if (bytes.size() - r.at != n)
                return;
            if (s.received[part]) {
                if (std::memcmp(s.blob.bytes.data() + offset, bytes.data() + r.at, n))
                    s.stage = Stage::Finished;
                return;
            }
            std::copy_n(bytes.data() + r.at, n, s.blob.bytes.data() + offset);
            s.received[part] = true;
            while (s.base < s.count && s.received[s.base])
                ++s.base;
            s.ack = 0;
            if (s.base == s.count) {
                if (hash(s.blob.bytes) != s.blob.checksum) {
                    s.stage = Stage::Finished;
                    return;
                }
                s.stage = Stage::Waiting;
            }
        } else if (type == Message::Control) {
            unsigned serial, stage;
            std::uint64_t cursor, delay;
            r.value(serial);
            r.value(stage);
            r.value(cursor);
            r.value(delay);
            if (r.at != bytes.size() || !serial || serial <= s.control_serial || delay > 1000000 ||
                cursor > std::numeric_limits<std::uint64_t>::max() - 250000 ||
                now > std::numeric_limits<std::uint64_t>::max() - delay)
                return;
            s.control_serial = serial;
            if (stage == unsigned(Stage::Finished)) {
                s.stage = Stage::Finished;
                return;
            }
            if (stage != unsigned(Stage::Playing) || !s.ready)
                return;
            const auto previous = s.stage == Stage::Playing ? elapsed(now, s.started) : 0;
            cursor = std::max(previous, cursor + std::min<std::uint64_t>(one_way, 250000));
            s.started = delay > one_way ? now + delay - one_way : (now > cursor ? now - cursor : 0);
            s.stage = Stage::Playing;
        }
    } catch (...) {
        if (state_)
            state_->stage = Stage::Finished;
    }
}
std::optional<Blob> Channel::take_received() {
    if (!state_ || state_->host || state_->taken || state_->stage != Stage::Waiting)
        return {};
    state_->taken = true;
    return std::move(state_->blob);
}
void Channel::decoded(bool accepted) {
    if (state_ && !state_->host && state_->taken && state_->stage == Stage::Waiting) {
        state_->ready = accepted;
        if (!accepted)
            state_->stage = Stage::Finished;
        state_->ack = 0;
    }
}
bool Channel::begin(std::uint64_t now, unsigned mask) {
    if (!state_ || !state_->host || !state_->published ||
        (state_->stage != Stage::Transferring && state_->stage != Stage::Waiting))
        return false;
    for (unsigned p = 1; p < 14; ++p)
        if ((mask & (1u << p)) && !state_->peers[p].ready)
            return false;
    state_->stage = Stage::Playing;
    state_->started = now + 500000;
    state_->control = 0;
    return true;
}
bool Channel::skip() {
    if (!state_ || !state_->host)
        return false;
    state_->stage = Stage::Finished;
    state_->published = true;
    state_->control = 0;
    return true;
}
Status Channel::status(std::uint64_t now, unsigned mask) const {
    if (!state_)
        return {};
    auto &s = *state_;
    Status out{s.stage, 0,
               s.stage == Stage::Playing ? elapsed(now, s.started) : 0,
               s.duration, s.taken};
    if (s.host && (s.stage == Stage::Transferring || s.stage == Stage::Waiting))
        for (unsigned p = 1; p < 14; ++p)
            if ((mask & (1u << p)) && !s.peers[p].ready)
                out.waiting_mask |= 1u << p;
    return out;
}
void Channel::service(std::uint64_t now, unsigned mask, const Send &send) {
    if (!state_ || !state_->round)
        return;
    auto &s = *state_;
    mask &= all_peers;
    if (!s.first)
        s.first = now;
    if ((s.stage == Stage::Idle || s.stage == Stage::Transferring || s.stage == Stage::Waiting) &&
        elapsed(now, s.first) >= timeout_us)
        s.stage = Stage::Finished;
    if (!s.host) {
        if (s.published && s.stage != Stage::Finished &&
            (!s.ack || elapsed(now, s.ack) >= 100000)) {
            auto w = message(Message::Ack, s.round);
            w.value(s.base);
            std::uint64_t bits = 0;
            for (unsigned i = 0; i < window && s.base + i < s.count; ++i)
                if (s.received[s.base + i])
                    bits |= 1ull << i;
            w.value(bits);
            w.value(s.ready);
            send(0, w.bytes);
            s.ack = now;
        }
        return;
    }
    if (!s.published && s.stage != Stage::Finished)
        return;
    if (!s.control || elapsed(now, s.control) >= 100000) {
        auto w =
            message((s.stage == Stage::Playing || s.stage == Stage::Finished) ? Message::Control
                                                                              : Message::Manifest,
                    s.round);
        if (s.stage == Stage::Playing || s.stage == Stage::Finished) {
            w.value(++s.control_serial);
            w.value(unsigned(s.stage));
            w.value(elapsed(now, s.started));
            w.value(s.started > now ? s.started - now : 0);
        } else {
            w.value(unsigned(s.blob.bytes.size()));
            w.value(s.blob.raw_size);
            w.value(s.count);
            w.value(s.blob.checksum);
            w.value(s.duration);
        }
        for (unsigned p = 1; p < 14; ++p)
            if (mask & (1u << p))
                send(p, w.bytes);
        s.control = now;
    }
    if (s.stage != Stage::Transferring && s.stage != Stage::Waiting)
        return;
    const auto delta = s.service ? std::min<std::uint64_t>(elapsed(now, s.service), 100000) : 16000;
    s.service = now;
    // One MiB/s aggregate, shared across all peers; bounded initial burst.
    s.credit = std::min<std::uint64_t>(32768, s.credit + delta * 1048576 / 1000000);
    bool progress = true;
    while (s.credit >= maximum_datagram && progress) {
        progress = false;
        for (unsigned peer = 1; peer < 14 && s.credit >= maximum_datagram; ++peer) {
            if (!(mask & (1u << peer)))
                continue;
            auto &p = s.peers[peer];
            if (p.ready)
                continue;
            for (unsigned i = 0; i < window && p.base + i < s.count; ++i) {
                if (p.mask & (1ull << i))
                    continue;
                unsigned part = p.base + i, k = part % window;
                if (p.part[k] == part && elapsed(now, p.sent[k]) < resend_us)
                    continue;
                auto w = message(Message::Chunk, s.round);
                w.value(part);
                const auto at = std::size_t(part) * chunk_bytes,
                           n = std::min<std::size_t>(chunk_bytes, s.blob.bytes.size() - at);
                w.bytes.insert(w.bytes.end(), s.blob.bytes.begin() + at,
                               s.blob.bytes.begin() + at + n);
                send(peer, w.bytes);
                p.part[k] = part;
                p.sent[k] = now;
                s.credit -= w.bytes.size();
                progress = true;
                break;
            }
        }
    }
    bool ready = true;
    for (unsigned p = 1; p < 14; ++p)
        if ((mask & (1u << p)) && !s.peers[p].ready)
            ready = false;
    if (ready)
        s.stage = Stage::Waiting;
}
}
