#include "rr64_prediction_replay.hpp"
#include "rr64_netplay.hpp"
#include "rr64_diagnostic_options.hpp"
#include "rr64_connection_address.hpp"
#include "rr64_local_race_options.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <mutex>
#include <random>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <WinSock2.h>
#include <WS2tcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

using SOCKET = int;
using socklen_type = socklen_t;
constexpr SOCKET INVALID_SOCKET = -1;
constexpr int SOCKET_ERROR = -1;
constexpr int WSAEWOULDBLOCK = EWOULDBLOCK;

inline int closesocket(SOCKET socket_handle) {
    return ::close(socket_handle);
}

inline int ioctlsocket(SOCKET socket_handle, long command, u_long *argument) {
    return ::ioctl(socket_handle, static_cast<unsigned long>(command), argument);
}

inline int WSAGetLastError() {
    return errno;
}
#endif

#ifdef _WIN32
using socklen_type = int;
#endif

namespace rr64::netplay {
namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kProtocolMagic = 0x52523634u; // RR64
constexpr std::uint16_t kProtocolVersion = 68; // Matching imported AI forecast and route recovery.
constexpr std::size_t kPlayerNameCapacity = 24;
constexpr auto kHelloInterval = std::chrono::milliseconds(500);
constexpr auto kStateInterval = std::chrono::milliseconds(25);
constexpr auto kSnapshotInterval = std::chrono::milliseconds(50);
constexpr auto kRaceStateInterval = std::chrono::milliseconds(16);
constexpr auto kRaceSnapshotInterval = std::chrono::milliseconds(8);
constexpr auto kWorldSnapshotInterval = std::chrono::milliseconds(25);
constexpr auto kRiderRepairInterval = std::chrono::milliseconds(100);
constexpr auto kPingInterval = std::chrono::seconds(1);
constexpr auto kPeerTimeout = std::chrono::seconds(5);
constexpr auto kConnectTimeout = std::chrono::seconds(5);
constexpr std::size_t kMaximumQueuedVoiceFrames = 128;
constexpr std::uint16_t kMaximumVoicePacketsPerSecond = 75;

enum class PacketType : std::uint8_t {
    Hello = 1,
    Welcome,
    ClientState,
    LobbySnapshot,
    Ping,
    Pong,
    Disconnect,
    ClientRiderState,
    RaceSnapshot,
    Voice,
    ConnectReject,
    WorldSnapshot,
    HitRequest, HitCommit, HitRequestAck, HitCommitAck,
    AuthorityBegin, AuthorityInput, AuthorityState, AuthorityReady, AuthorityWorld, AuthorityItems, AuthorityHazards,
    Highlight, AuthorityMk64Riders, AuthorityMk64Objects,
};

#pragma pack(push, 1)
struct PacketHeader {
    std::uint32_t magic = kProtocolMagic;
    std::uint16_t version = kProtocolVersion;
    PacketType type = PacketType::Hello;
    std::uint8_t reserved = 0;
    std::uint16_t size = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t sequence = 0;
    std::uint64_t session = 0;
};

struct AuthorityBeginPacket { PacketHeader header{}; std::uint32_t round=0,humans=0,released=0,finish_mode=0; std::uint64_t finish_tick=0; };
struct AuthorityReadyPacket { PacketHeader header{}; std::uint32_t round=0; };
struct AuthorityInputPacket { PacketHeader header{}; authority::InputBatch batch{}; };
static_assert(sizeof(AuthorityInputPacket)<1200);

struct HitPacket { PacketHeader header{}; HitEvent event{}; };

struct HelloPacket {
    PacketHeader header{};
    char player_name[kPlayerNameCapacity]{};
};
enum class RejectReason : std::uint16_t { Full = 1, Busy = 2, Version = 3 };
struct ConnectRejectPacket {
    PacketHeader header{};
    RejectReason reason{};
    std::uint16_t expected_version = kProtocolVersion;
};

struct WelcomePacket {
    PacketHeader header{};
    std::uint8_t assigned_slot = kInvalidSlot;
    std::uint8_t maximum_players = kMaximumPlayers;
    std::uint8_t phase = static_cast<std::uint8_t>(Phase::Lobby);
    std::uint8_t replicated_riders = 0;
};

struct ClientStatePacket {
    PacketHeader header{};
    online_flow::PostRaceState postrace_ack{};
    std::uint32_t course_revision=0;
    race_pack::Identity course{};
    std::uint8_t course_compatibility = static_cast<std::uint8_t>(race_pack::Compatibility::Pending);
    online_flow::Selection selection{};
    std::uint8_t slot = kInvalidSlot;
    std::uint8_t ready = 0;
    std::uint8_t character = 0;
    std::uint8_t track = 0;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;
    char player_name[kPlayerNameCapacity]{};
};

struct WirePlayer {
    online_flow::Selection selection{};
    std::uint8_t course_compatibility = static_cast<std::uint8_t>(race_pack::Compatibility::Pending);
    std::uint8_t connected = 0;
    std::uint8_t ready = 0;
    std::uint8_t character = 0;
    std::uint8_t track = 0;
    std::uint16_t buttons = 0;
    std::int8_t stick_x = 0;
    std::int8_t stick_y = 0;
    std::uint16_t ping_ms = 0;
    char name[kPlayerNameCapacity]{};
};

struct WireGameSetup {
    race_pack::Identity course{};
    std::uint32_t start_requested = 0;
    std::uint32_t random_seed = 0;
    std::uint32_t revision = 0;
    std::uint16_t transition_buttons = 0;
    std::uint8_t valid = 0;
    std::uint8_t reserved = 0;
    std::uint32_t words[kGameSetupWordCount]{};
    std::uint32_t race_options = 0;
};

struct LobbySnapshotPacket {
    PacketHeader header{};
    online_flow::PostRaceState postrace{};
    std::uint8_t phase = static_cast<std::uint8_t>(Phase::Lobby);
    std::uint8_t connected_players = 0;
    std::uint8_t replicated_riders = 0;
    std::uint8_t reserved = 0;
    WireGameSetup game_setup{};
    WirePlayer players[kMaximumPlayers]{};
};

struct WireRiderState {
    std::uint8_t active = 0;
    std::uint8_t animation = 0;
    std::uint8_t weapon = 0;
    std::uint8_t character = 0;
    std::uint8_t bike = 0;
    std::uint8_t reserved = 0;
    std::uint16_t health = 0;
    std::uint16_t flags = 0;
    std::uint16_t reserved2 = 0;
    std::uint32_t tick = 0;
    std::uint64_t sample_time_us = 0;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float front_wheel_x = 0.0f;
    float front_wheel_y = 0.0f;
    float front_wheel_z = 0.0f;
    float rear_wheel_x = 0.0f;
    float rear_wheel_y = 0.0f;
    float rear_wheel_z = 0.0f;
    std::uint32_t rider_position_valid = 0;
    float rider_x = 0.0f, rider_y = 0.0f, rider_z = 0.0f;
    RiderRootState root{};
    float bike_lean = 0.0f;
    float front_suspension = 0.0f;
    float rear_suspension = 0.0f;
};

struct ClientRiderStatePacket {
    PacketHeader header{};
    std::uint32_t round = 0;
    std::uint8_t slot = kInvalidSlot;
    std::uint8_t reserved[3]{};
    WireRiderState state{};
};

// Bounded parts retain room for physics fields without IP fragmentation.
constexpr unsigned kAuthorityPartRiders=1;
constexpr unsigned kAuthorityParts=(kMaximumPlayers+kAuthorityPartRiders-1)/kAuthorityPartRiders;
static_assert(kAuthorityPartRiders==1 && kAuthorityParts<32);
unsigned authority_payload_mask(const AuthorityFrame &frame) {
    unsigned mask=0;
    for(unsigned s=0;s<kMaximumPlayers;++s)if(frame.riders[s].active || frame.outcomes[s].valid)mask|=1u<<s;
    return mask;
}
struct AuthorityStatePacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    authority::NativeTiming timing{};
    std::uint32_t part=0;
    std::uint32_t payload_mask=0,world_mask=0;
    WireRiderState riders[kAuthorityPartRiders]{};
    authority::Outcome outcomes[kAuthorityPartRiders]{};
    authority::RiderDynamics dynamics[kAuthorityPartRiders]{};
    std::uint32_t cop_mode=0;
    float cop_win_age=-1;
};
static_assert(sizeof(AuthorityStatePacket)<1200);
// Traffic and riders share the exact simulation stamp, but use separate
// sub-MTU fragments. No half-world frame is made visible to the game.
struct AuthorityWorldPacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    std::uint32_t part=0,payload_mask=0,world_mask=0;
    std::array<world_sync::Traffic,world_sync::batch_size> traffic{};
};
static_assert(sizeof(AuthorityWorldPacket)<1200);
// Items are one additional world fragment, required only for imported courses
// that actually contain boxes. Share assembly masks/stamps with traffic rather
// than exposing an independently timed reward/visibility channel.
constexpr unsigned kAuthorityItemsBit=1u<<world_sync::batches;
constexpr unsigned kAuthorityHazardFirst=world_sync::batches+1;
constexpr unsigned kHazardsPerPacket=16;
constexpr unsigned kHazardParts=(kMaximumCourseHazards+kHazardsPerPacket-1)/kHazardsPerPacket;
constexpr unsigned kAuthorityMk64First=kAuthorityHazardFirst+kHazardParts;
constexpr unsigned kAuthorityMk64RidersBit=1u<<kAuthorityMk64First;
constexpr unsigned kMk64ObjectsPerPacket=16;
constexpr unsigned kMk64ObjectParts=(mk64_items::object_capacity+kMk64ObjectsPerPacket-1)/kMk64ObjectsPerPacket;
constexpr unsigned kAuthorityWorldParts=kAuthorityMk64First+1+kMk64ObjectParts;
static_assert(kAuthorityWorldParts<32);
struct AuthorityItemsPacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    std::uint32_t payload_mask=0,world_mask=0;
    CourseItemState items{};
};
static_assert(sizeof(AuthorityItemsPacket)<1200);
struct AuthorityHazardsPacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    std::uint32_t payload_mask=0,world_mask=0,clock=0;
    std::uint16_t count=0,part=0;
    std::array<CourseHazardPose,kHazardsPerPacket> poses{};
};
static_assert(sizeof(AuthorityHazardsPacket)<1200);
// Item effects and objects join the existing complete-world assembly. A metadata
// fragment is sent even when every object is gone, so retirement is explicit.
struct Mk64Metadata {
    std::uint32_t enabled=0,clock=0,next_generation=0,random=0;
    bool operator==(const Mk64Metadata &) const = default;
};
struct AuthorityMk64RidersPacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    std::uint32_t payload_mask=0,world_mask=0;
    Mk64Metadata meta{};
    std::array<mk64_items::RiderState,mk64_items::racer_capacity> riders{};
};
struct AuthorityMk64ObjectsPacket {
    PacketHeader header{};
    authority::Stamp stamp{};
    std::uint32_t payload_mask=0,world_mask=0,part=0;
    Mk64Metadata meta{};
    std::array<mk64_items::Object,kMk64ObjectsPerPacket> objects{};
};
static_assert(sizeof(AuthorityMk64RidersPacket)<1200);
static_assert(sizeof(AuthorityMk64ObjectsPacket)<1200);
Mk64Metadata mk64_metadata(const mk64_items::Snapshot &state) {
    return {state.enabled,state.clock,state.next_generation,state.random};
}
bool valid_mk64_metadata(const Mk64Metadata &meta) {
    return meta.enabled==1 && meta.clock<=30000000 && meta.random;
}
unsigned mk64_world_mask(const mk64_items::Snapshot &state) {
    if(!state.enabled)return 0;
    unsigned mask=kAuthorityMk64RidersBit;
    for(unsigned i=0;i<mk64_items::object_capacity;++i)
        if(state.objects[i].generation)mask|=1u<<(kAuthorityMk64First+1+i/kMk64ObjectsPerPacket);
    return mask;
}
constexpr unsigned kAuthorityMk64Mask=((1u<<(1+kMk64ObjectParts))-1u)<<kAuthorityMk64First;
unsigned hazard_world_mask(unsigned count){
    if(!count || count>kMaximumCourseHazards)return 0;
    const unsigned parts=(count+kHazardsPerPacket-1)/kHazardsPerPacket;
    return ((1u<<parts)-1u)<<kAuthorityHazardFirst;
}
unsigned authority_world_mask(const AuthorityFrame &frame){
    unsigned mask=0;
    for(unsigned i=0;i<world_sync::capacity;++i)if(frame.traffic[i].active)
        mask|=1u<<(i/world_sync::batch_size);
    // An empty roster must explicitly retire old traffic even with item boxes.
    return (mask?mask:1u) | (frame.course_items.count?kAuthorityItemsBit:0u) |
        hazard_world_mask(frame.course_hazards.count) | mk64_world_mask(frame.mk64_items);
}


struct RaceSnapshotPacket {
    PacketHeader header{};
    std::uint32_t round = 0;
    std::uint32_t host_tick = 0;
    std::uint32_t slot_mask = 0;
    WireRiderState riders[4]{};
};

struct WorldSnapshotPacket {
    PacketHeader header{};
    world_sync::Batch batch{};
};

struct PingPacket {
    PacketHeader header{};
    std::uint8_t slot = kInvalidSlot;
    std::uint8_t reserved[3]{};
    std::uint32_t nonce = 0;
};

struct VoicePacket {
    PacketHeader header{};
    std::uint8_t speaker_slot = kInvalidSlot;
    std::uint8_t reserved = 0;
    std::uint16_t payload_size = 0;
    std::uint32_t voice_sequence = 0;
    std::uint8_t payload[kVoicePayloadCapacity]{};
};
#pragma pack(pop)

struct AuthorityAssembly {
    AuthorityFrame frame{};
    unsigned mask=0;
    unsigned payload_mask=0,world_mask=0,received_world=0;
};

struct InputState {
    std::uint16_t buttons = 0;
    float stick_x = 0.0f;
    float stick_y = 0.0f;
};

struct Peer {
    bool connected = false;
    sockaddr_in endpoint{};
    Clock::time_point last_seen{};
    Clock::time_point ping_sent{};
    std::uint32_t ping_nonce = 0;
    std::uint32_t last_state_sequence = 0;
    Clock::time_point voice_window_start{};
    std::uint16_t voice_packets_in_window = 0;
};

struct HitQueueEntry { HitEvent event{}; Clock::time_point sent{}; };
struct HitChannel {
    std::uint32_t round=0, next_request=1, received_commit=0;
    std::array<std::uint32_t,kMaximumPlayers> received_request{},next_commit{};
    std::deque<HitQueueEntry> proposals;
    std::array<std::deque<HitQueueEntry>,kMaximumPlayers> commits;
    std::deque<HitEvent> delivered;
};
struct Session {
    online_flow::PostRaceState postrace{};
    std::array<online_flow::PostRaceState,kMaximumPlayers> postrace_ack{};
    highlight_network::Channel highlights;
    bool authoritative=false;
    unsigned authority_loaded=0;
    unsigned authority_finish_mode=0;
    std::uint64_t authority_finish_tick=0;
    bool authority_released=false;
    std::uint32_t authority_local_sequence=0;
    std::uint32_t authority_round=0,authority_humans=0;
    authority::HostRound authority_host{};
    authority::ClientHistory authority_client{};
    Clock::time_point authority_sent{};
    AuthorityFrame authority_frame{};
    std::array<AuthorityAssembly,8> authority_assemblies{};
    authority::Stamp authority_replay_stamp{};
    std::uint32_t authority_replay_last=0;
    std::uint64_t authority_replay_ticket=0;
    HitChannel hits{};
    bool host_disconnected = false;
    Clock::time_point host_disconnected_at{};
    Config config{};
    SOCKET socket = INVALID_SOCKET;
    sockaddr_in host_endpoint{};
    std::uint64_t token = 0;
    std::uint32_t sequence = 1;
    std::uint8_t local_slot = kInvalidSlot;
    Phase phase = Phase::Offline;
    std::array<PlayerInfo, kMaximumPlayers> players{};
    std::array<InputState, kMaximumPlayers> inputs{};
    std::array<RiderState, kMaximumPlayers> riders{};
    std::array<RiderState, kMaximumPlayers> previous_riders{};
    std::array<Peer, kMaximumPlayers> peers{};
    GameSetupState game_setup{};
    std::array<std::uint32_t,kMaximumPlayers> course_ack{};
    std::uint32_t setup_serial = 0;
    std::uint32_t race_player_mask = 0;
    world_sync::Snapshot world{};
    world_sync::Receiver world_receiver{};
    Clock::time_point last_hello{};
    Clock::time_point last_state{};
    Clock::time_point last_snapshot{};
    Clock::time_point last_race_state{};
    Clock::time_point last_race_snapshot{};
    Clock::time_point last_world_snapshot{};
    std::array<std::array<std::uint32_t,kMaximumPlayers>,kMaximumPlayers> sent_rider_ticks{};
    std::array<std::array<Clock::time_point,kMaximumPlayers>,kMaximumPlayers> sent_rider_at{};
    Clock::time_point last_ping{};
    std::uint32_t client_ping_nonce = 0;
    Clock::time_point client_ping_sent{};
    std::string message = "Offline";
    std::uint32_t last_lobby_snapshot_sequence = 0;
    std::uint32_t last_race_snapshot_sequence = 0;
    std::uint32_t local_voice_sequence = 0;
    std::array<std::uint32_t, kMaximumPlayers> last_voice_sequences{};
    std::deque<ReceivedVoiceFrame> received_voice_frames{};
    bool replicated_riders = false;
    Clock::time_point connect_started{}, last_host_seen{};
    std::uint64_t hello_attempts = 0, received_packets = 0;
    int last_socket_error = 0;
};

std::mutex g_mutex;
std::uint64_t g_authority_replay_ticket=0; // never reused across session resets
Session g_session{};
struct AuthorityReadFrame {bool enabled=false;std::uint32_t token=0,round=0;AuthorityFrame frame{};};
thread_local AuthorityReadFrame authority_read;
const AuthorityFrame &authority_read_frame_locked(){
    if(authority_read.enabled && authority_read.token==g_session.token &&
       authority_read.round==g_session.authority_round && g_session.config.mode==Mode::Join)
        return authority_read.frame;
    return g_session.authority_frame;
}

bool g_winsock_started = false;

bool is_host_locked() {
    return g_session.config.mode == Mode::Host;
}

bool is_client_locked() {
    return g_session.config.mode == Mode::Join;
}

std::uint64_t make_session_token() {
    const auto now = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
    std::random_device random;
    return now ^ (static_cast<std::uint64_t>(random()) << 32) ^ random();
}

void copy_string(char *destination, std::size_t capacity, const std::string &source) {
    if (capacity == 0) {
        return;
    }
    const std::size_t length = std::min(capacity - 1, source.size());
    std::memcpy(destination, source.data(), length);
    destination[length] = '\0';
}

std::string read_string(const char *source, std::size_t capacity) {
    const void *end = std::memchr(source, '\0', capacity);
    const std::size_t length = end == nullptr ? capacity : static_cast<const char *>(end) - source;
    return std::string(source, length);
}

bool finite_rider_state(const RiderState &state) {
    if(!valid_attack_visual(state.root.attack)) return false;
    if (state.host_ai && (state.character>44 || state.bike>31)) return false;
    if (!std::isfinite(state.root.durability) || !std::isfinite(state.root.durability_capacity)) return false;
    if (state.root.equipment_valid>1 || (state.root.equipment_valid && (state.weapon<1 || state.weapon>14))) return false;
    if (state.root.valid > 1 || state.root.bike_attached > 1 ||
        state.root.rider_attached > 1 || state.root.ejected > 1 || state.root.vault_latch > 3 ||
        !std::isfinite(state.root.bike_height) || !std::isfinite(state.root.rider_height)) return false;
    for (const auto &v : {state.root.bike_origin,state.root.bike_velocity,state.root.bike_motion,state.root.rider_velocity,state.root.rider_anchor})
        for(float x:v) if(!std::isfinite(x)) return false;
    if (!std::isfinite(state.root.rider_impact_reserve)) return false;
    for (float x:state.root.bike_display_angles) if (!std::isfinite(x)) return false;
    for (float x:state.root.bike_rotation) if (!std::isfinite(x)) return false;
    for (float x:state.root.rider_rotation) if (!std::isfinite(x)) return false;
    return state.rider_position_valid <= 1u && std::isfinite(state.rider_x) &&
           std::isfinite(state.rider_y) && std::isfinite(state.rider_z) && std::isfinite(state.position_x) && std::isfinite(state.position_y) &&
           std::isfinite(state.position_z) && std::isfinite(state.front_wheel_x) &&
           std::isfinite(state.front_wheel_y) && std::isfinite(state.front_wheel_z) &&
           std::isfinite(state.rear_wheel_x) && std::isfinite(state.rear_wheel_y) &&
           std::isfinite(state.rear_wheel_z) && std::isfinite(state.bike_lean) &&
           std::isfinite(state.front_suspension) && std::isfinite(state.rear_suspension);
}

WireRiderState to_wire(const RiderState &state) {
    WireRiderState wire{};
    wire.root = state.root;
    wire.active = state.active ? 1 : 0;
    wire.reserved = state.host_ai ? 1 : 0;
    wire.animation = state.animation;
    wire.weapon = state.weapon;
    wire.character = state.character;
    wire.bike = state.bike;
    wire.health = state.health;
    wire.flags = state.flags;
    wire.tick = state.tick;
    wire.sample_time_us = state.sample_time_us;
    wire.position_x = state.position_x;
    wire.position_y = state.position_y;
    wire.position_z = state.position_z;
    wire.front_wheel_x = state.front_wheel_x;
    wire.front_wheel_y = state.front_wheel_y;
    wire.front_wheel_z = state.front_wheel_z;
    wire.rear_wheel_x = state.rear_wheel_x;
    wire.rear_wheel_y = state.rear_wheel_y;
    wire.rear_wheel_z = state.rear_wheel_z;
    wire.rider_position_valid = state.rider_position_valid;
    wire.rider_x = state.rider_x;
    wire.rider_y = state.rider_y;
    wire.rider_z = state.rider_z;
    wire.bike_lean = state.bike_lean;
    wire.front_suspension = state.front_suspension;
    wire.rear_suspension = state.rear_suspension;
    return wire;
}

RiderState from_wire(const WireRiderState &wire) {
    RiderState state{};
    state.root = wire.root;
    state.active = wire.active != 0;
    state.host_ai = wire.reserved != 0;
    state.animation = wire.animation;
    state.weapon = wire.weapon;
    state.character = wire.character;
    state.bike = wire.bike;
    state.health = wire.health;
    state.flags = wire.flags;
    state.tick = wire.tick;
    state.sample_time_us = wire.sample_time_us;
    state.position_x = wire.position_x;
    state.position_y = wire.position_y;
    state.position_z = wire.position_z;
    state.front_wheel_x = wire.front_wheel_x;
    state.front_wheel_y = wire.front_wheel_y;
    state.front_wheel_z = wire.front_wheel_z;
    state.rear_wheel_x = wire.rear_wheel_x;
    state.rear_wheel_y = wire.rear_wheel_y;
    state.rear_wheel_z = wire.rear_wheel_z;
    state.rider_position_valid = wire.rider_position_valid;
    state.rider_x = wire.rider_x;
    state.rider_y = wire.rider_y;
    state.rider_z = wire.rider_z;
    state.bike_lean = wire.bike_lean;
    state.front_suspension = wire.front_suspension;
    state.rear_suspension = wire.rear_suspension;
    return state;
}

float blend(float from, float to, float alpha) {
    return from + (to - from) * alpha;
}

RiderState interpolate(const RiderState &previous, const RiderState &current, float alpha) {
    // A root snapshot includes discrete attachment and orientation state.
    // Never combine its current root with positions from an earlier tick.
    if (current.root.valid || !previous.active || previous.tick >= current.tick) {
        return current;
    }
    RiderState result = current;
    result.position_x = blend(previous.position_x, current.position_x, alpha);
    result.position_y = blend(previous.position_y, current.position_y, alpha);
    result.position_z = blend(previous.position_z, current.position_z, alpha);
    result.front_wheel_x = blend(previous.front_wheel_x, current.front_wheel_x, alpha);
    result.front_wheel_y = blend(previous.front_wheel_y, current.front_wheel_y, alpha);
    result.front_wheel_z = blend(previous.front_wheel_z, current.front_wheel_z, alpha);
    result.rear_wheel_x = blend(previous.rear_wheel_x, current.rear_wheel_x, alpha);
    result.rear_wheel_y = blend(previous.rear_wheel_y, current.rear_wheel_y, alpha);
    result.rear_wheel_z = blend(previous.rear_wheel_z, current.rear_wheel_z, alpha);
    if (previous.rider_position_valid && current.rider_position_valid) {
        result.rider_x = blend(previous.rider_x, current.rider_x, alpha);
        result.rider_y = blend(previous.rider_y, current.rider_y, alpha);
        result.rider_z = blend(previous.rider_z, current.rider_z, alpha);
    }
    result.bike_lean = blend(previous.bike_lean, current.bike_lean, alpha);
    result.front_suspension = blend(previous.front_suspension, current.front_suspension, alpha);
    result.rear_suspension = blend(previous.rear_suspension, current.rear_suspension, alpha);
    return result;
}

static_assert(sizeof(LobbySnapshotPacket) <= 1200, "Lobby snapshots must avoid UDP fragmentation");
static_assert(sizeof(RaceSnapshotPacket) <= 1200, "Race snapshots must avoid UDP fragmentation");
static_assert(sizeof(WorldSnapshotPacket)<=1200,"World batches must avoid fragmentation");
static_assert(sizeof(VoicePacket) <= 1200, "Voice packets must avoid UDP fragmentation");

template <typename Packet> void initialize_packet(Packet &packet, PacketType type) {
    packet.header.magic = kProtocolMagic;
    packet.header.version = kProtocolVersion;
    packet.header.type = type;
    packet.header.size = static_cast<std::uint16_t>(sizeof(Packet));
    packet.header.sequence = g_session.sequence++;
    packet.header.session = g_session.token;
}

bool endpoint_equal(const sockaddr_in &left, const sockaddr_in &right) {
    return left.sin_family == right.sin_family && left.sin_port == right.sin_port &&
           left.sin_addr.s_addr == right.sin_addr.s_addr;
}

void close_socket_locked() {
    if (g_session.socket != INVALID_SOCKET) {
        closesocket(g_session.socket);
        g_session.socket = INVALID_SOCKET;
    }
}
void host_disconnected_locked() {
    // Preserve the last race/camera identity for the terminal notification.
    // A dead host is not permission to resume as local split-screen players.
    g_session.host_disconnected = true;
    g_session.host_disconnected_at = Clock::now();
    g_session.message = "Host Disconnected";
    g_session.inputs = {};
    close_socket_locked();
}
void connection_failure_locked(const std::string &message) {
    std::fprintf(
        stderr, "[RR64-NET] failed: %s hello-attempts=%llu received=%llu last-socket-error=%d\n",
        message.c_str(), static_cast<unsigned long long>(g_session.hello_attempts),
        static_cast<unsigned long long>(g_session.received_packets), g_session.last_socket_error);
    std::fflush(stderr);
    close_socket_locked();
    g_session.phase = Phase::Offline;
    g_session.config.mode = Mode::Offline;
    g_session.local_slot = kInvalidSlot;
    g_session.token = 0;
    g_session.players = {};
    g_session.inputs = {};
    g_session.riders = {};
    g_session.previous_riders = {};
    g_session.message = message;
}
void record_socket_error_locked(const char *operation, int error) {
    if (error == WSAEWOULDBLOCK)
        return;
    if (g_session.last_socket_error != error) {
        std::fprintf(stderr, "[RR64-NET] %s error=%d\n", operation, error);
        std::fflush(stderr);
    }
    g_session.last_socket_error = error;
}

bool start_winsock_locked() {
    if (g_winsock_started) {
        return true;
    }
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return false;
    }
#endif
    g_winsock_started = true;
    return true;
}

bool create_socket_locked(bool bind_host, std::uint16_t port) {
    if (!start_winsock_locked()) {
        g_session.message = "Could not initialize Windows networking";
        return false;
    }

    g_session.socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (g_session.socket == INVALID_SOCKET) {
        g_session.message = "Could not create UDP socket";
        return false;
    }

    u_long nonblocking = 1;
    if (ioctlsocket(g_session.socket, FIONBIO, &nonblocking) == SOCKET_ERROR) {
        close_socket_locked();
        g_session.message = "Could not configure UDP socket";
        return false;
    }

    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port = htons(bind_host ? port : 0);
#ifdef _WIN32
    if (bind_host) {
        BOOL exclusive = TRUE;
        if (setsockopt(g_session.socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char *>(&exclusive),
                       sizeof(exclusive)) == SOCKET_ERROR) {
            record_socket_error_locked("exclusive-bind", WSAGetLastError());
            close_socket_locked();
            g_session.message = "Could not reserve the hosting port.";
            return false;
        }
    }
#endif
    if (bind(g_session.socket, reinterpret_cast<const sockaddr *>(&local), sizeof(local)) ==
        SOCKET_ERROR) {
        record_socket_error_locked("bind", WSAGetLastError());
        close_socket_locked();
        g_session.message = "Could not bind UDP port " + std::to_string(port);
        return false;
    }
    if (rr64::diagnostics::network_detail_enabled()) {
        socklen_type length = sizeof(local);
        getsockname(g_session.socket, reinterpret_cast<sockaddr *>(&local), &length);
        std::fprintf(stderr, "[RR64-NET] socket role=%s local-udp-port=%u protocol=%u\n",
                     bind_host ? "host" : "join", ntohs(local.sin_port), kProtocolVersion);
        std::fflush(stderr);
    }
    return true;
}

bool resolve_host(const std::string &address, std::uint16_t port, sockaddr_in &endpoint) {
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_protocol = IPPROTO_UDP;
    addrinfo *results = nullptr;
    const std::string port_text = std::to_string(port);
    if (getaddrinfo(address.c_str(), port_text.c_str(), &hints, &results) != 0 ||
        results == nullptr) {
        return false;
    }
    endpoint = *reinterpret_cast<sockaddr_in *>(results->ai_addr);
    freeaddrinfo(results);
    return true;
}

template <typename Packet>
void send_packet_locked(const Packet &packet, const sockaddr_in &endpoint) {
    if (g_session.socket == INVALID_SOCKET) {
        return;
    }
    const int sent = sendto(g_session.socket, reinterpret_cast<const char *>(&packet),
                            static_cast<int>(sizeof(Packet)), 0,
                            reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint));
    if (sent == SOCKET_ERROR)
        record_socket_error_locked("send", WSAGetLastError());
}

// A running round has an immutable human roster. Losing one participant must
// end that round rather than leave a held input or a loading barrier forever.
// Called under g_mutex, including from receive/timeout handling.
void stop_authority_locked(const char *reason) {
    if(!g_session.authoritative || g_session.host_disconnected)return;
    PacketHeader packet{};packet.type=PacketType::Disconnect;packet.size=sizeof(packet);
    packet.sequence=g_session.sequence++;packet.session=g_session.token;
    if(is_host_locked()) {
        for(unsigned slot=1;slot<kMaximumPlayers;++slot)
            if(g_session.peers[slot].connected)send_packet_locked(packet,g_session.peers[slot].endpoint);
    } else if(is_client_locked())send_packet_locked(packet,g_session.host_endpoint);
    host_disconnected_locked();
    g_session.message="Online sync stopped";
    std::fprintf(stderr,"[RR64-NET] authority stopped: %s\n",reason?reason:"unknown");
}

std::size_t voice_packet_size(const VoicePacket &packet) {
    return offsetof(VoicePacket, payload) + packet.payload_size;
}

bool valid_voice_packet(const VoicePacket &packet, int size) {
    return packet.payload_size > 0 && packet.payload_size <= kVoicePayloadCapacity &&
           size == static_cast<int>(voice_packet_size(packet));
}

void send_voice_packet_locked(VoicePacket &packet, const sockaddr_in &endpoint) {
    if (g_session.socket == INVALID_SOCKET) {
        return;
    }
    const std::size_t size = voice_packet_size(packet);
    packet.header.size = static_cast<std::uint16_t>(size);
    sendto(g_session.socket, reinterpret_cast<const char *>(&packet), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr *>(&endpoint), sizeof(endpoint));
}

void enqueue_voice_frame_locked(const VoicePacket &packet) {
    if (g_session.received_voice_frames.size() >= kMaximumQueuedVoiceFrames) {
        g_session.received_voice_frames.pop_front();
    }
    ReceivedVoiceFrame frame{};
    frame.speaker_slot = packet.speaker_slot;
    frame.sequence = packet.voice_sequence;
    frame.payload_size = packet.payload_size;
    std::copy_n(packet.payload, packet.payload_size, frame.payload.begin());
    g_session.received_voice_frames.push_back(std::move(frame));
}

std::uint8_t connected_count_locked() {
    return static_cast<std::uint8_t>(
        std::count_if(g_session.players.begin(), g_session.players.end(),
                      [](const PlayerInfo &player) { return player.connected; }));
}

std::uint8_t find_peer_slot_locked(const sockaddr_in &endpoint) {
    for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
        if (g_session.peers[slot].connected &&
            endpoint_equal(g_session.peers[slot].endpoint, endpoint)) {
            return slot;
        }
    }
    return kInvalidSlot;
}

std::uint8_t allocate_peer_slot_locked() {
    for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
        if (!g_session.peers[slot].connected) {
            return slot;
        }
    }
    return kInvalidSlot;
}

void send_welcome_locked(std::uint8_t slot) {
    WelcomePacket packet{};
    initialize_packet(packet, PacketType::Welcome);
    packet.assigned_slot = slot;
    packet.phase = static_cast<std::uint8_t>(g_session.phase);
    packet.replicated_riders = g_session.replicated_riders ? 1 : 0;
    send_packet_locked(packet, g_session.peers[slot].endpoint);
}

void send_hello_locked(const Clock::time_point now) {
    HelloPacket packet{};
    initialize_packet(packet, PacketType::Hello);
    packet.header.session = 0;
    copy_string(packet.player_name, sizeof(packet.player_name), g_session.config.player_name);
    send_packet_locked(packet, g_session.host_endpoint);
    g_session.last_hello = now;
    ++g_session.hello_attempts;
    if (rr64::diagnostics::network_detail_enabled() &&
        (g_session.hello_attempts == 1 || g_session.hello_attempts % 10 == 0)) {
        std::fprintf(stderr, "[RR64-NET] hello attempt=%llu received=%llu\n",
                     static_cast<unsigned long long>(g_session.hello_attempts),
                     static_cast<unsigned long long>(g_session.received_packets));
        std::fflush(stderr);
    }
}

void send_client_state_locked(const Clock::time_point now) {
    if (g_session.local_slot == kInvalidSlot) {
        return;
    }
    const std::uint8_t slot = g_session.local_slot;
    ClientStatePacket packet{};
    if(g_session.local_slot<kMaximumPlayers) {
        packet.course_revision=g_session.game_setup.valid ? g_session.game_setup.revision : 0;
        packet.course=g_session.game_setup.course;
        packet.course_compatibility=static_cast<std::uint8_t>(g_session.players[slot].course_compatibility);
    }

    initialize_packet(packet, PacketType::ClientState);
    packet.slot = slot;
    packet.selection = g_session.players[slot].selection;
    packet.postrace_ack = g_session.postrace_ack[slot];
    packet.ready = g_session.players[slot].ready ? 1 : 0;
    packet.character = g_session.players[slot].character;
    packet.track = g_session.players[slot].track;
    packet.buttons = g_session.inputs[slot].buttons;
    packet.stick_x = static_cast<std::int8_t>(
        std::lround(std::clamp(g_session.inputs[slot].stick_x, -1.0f, 1.0f) * 127.0f));
    packet.stick_y = static_cast<std::int8_t>(
        std::lround(std::clamp(g_session.inputs[slot].stick_y, -1.0f, 1.0f) * 127.0f));
    copy_string(packet.player_name, sizeof(packet.player_name), g_session.players[slot].name);
    send_packet_locked(packet, g_session.host_endpoint);
    g_session.last_state = now;
}

void send_snapshot_locked(const Clock::time_point now) {
    LobbySnapshotPacket packet{};
    packet.postrace = g_session.postrace;
    initialize_packet(packet, PacketType::LobbySnapshot);
    packet.phase = static_cast<std::uint8_t>(g_session.phase);
    packet.connected_players = connected_count_locked();
    packet.reserved = static_cast<std::uint8_t>(
        std::clamp<unsigned>(g_session.config.maximum_players, 2u, kMaximumPlayers));
    packet.replicated_riders = g_session.replicated_riders ? 1 : 0;
    packet.game_setup.revision = g_session.game_setup.revision;
    packet.game_setup.transition_buttons = g_session.game_setup.transition_buttons;
    packet.game_setup.race_options = g_session.game_setup.race_options;
    packet.game_setup.random_seed = g_session.game_setup.random_seed;
    packet.game_setup.course = g_session.game_setup.course;
    packet.game_setup.start_requested = g_session.game_setup.start_requested;
    packet.game_setup.valid = g_session.game_setup.valid ? 1 : 0;
    std::copy(g_session.game_setup.words.begin(), g_session.game_setup.words.end(),
              packet.game_setup.words);
    for (std::uint8_t slot = 0; slot < kMaximumPlayers; ++slot) {
        const PlayerInfo &player = g_session.players[slot];
        WirePlayer &wire = packet.players[slot];
        wire.selection = player.selection;
        wire.course_compatibility = static_cast<std::uint8_t>(player.course_compatibility);
        wire.connected = player.connected ? 1 : 0;
        wire.ready = player.ready ? 1 : 0;
        wire.character = player.character;
        wire.track = player.track;
        wire.buttons = g_session.inputs[slot].buttons;
        wire.stick_x = static_cast<std::int8_t>(
            std::lround(std::clamp(g_session.inputs[slot].stick_x, -1.0f, 1.0f) * 127.0f));
        wire.stick_y = static_cast<std::int8_t>(
            std::lround(std::clamp(g_session.inputs[slot].stick_y, -1.0f, 1.0f) * 127.0f));
        wire.ping_ms = player.ping_ms;
        copy_string(wire.name, sizeof(wire.name), player.name);
    }
    for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
        if (g_session.peers[slot].connected) {
            send_packet_locked(packet, g_session.peers[slot].endpoint);
        }
    }
    g_session.last_snapshot = now;
}

// Keep the configured cadence despite late polling. Discard missed slots, not
// the phase of the schedule; never burst-send obsolete snapshots to catch up.
void advance_race_deadline(Clock::time_point &last,const Clock::time_point now,
                           const std::chrono::milliseconds interval) {
    if(now>=last+interval) last+=interval*((now-last)/interval);
}

void send_client_rider_state_locked(const Clock::time_point now) {
    if(g_session.authoritative)return;
    if (g_session.local_slot >= kMaximumPlayers) {
        return;
    }
    const RiderState &state = g_session.riders[g_session.local_slot];
    if (!state.active || !finite_rider_state(state)) {
        return;
    }
    ClientRiderStatePacket packet{};
    initialize_packet(packet, PacketType::ClientRiderState);
    packet.slot = g_session.local_slot;
    packet.round = g_session.game_setup.revision;
    packet.state = to_wire(state);
    send_packet_locked(packet, g_session.host_endpoint);
    advance_race_deadline(g_session.last_race_state,now,kRaceStateInterval);
}

void send_race_snapshot_locked(const Clock::time_point now) {
    // This is the legacy owner-state transport. Authority mode has its own
    // complete-frame channel; the mode flag is not the host/client role.
    if(g_session.authoritative)return;
    // Forward fresh actors promptly, without rebroadcasting every unchanged
    // actor or echoing a player's own state. Each record remains self-contained;
    // loss never creates a dependency on an earlier delta. Periodic repair also
    // delivers the final state if its last movement packet was lost.
    for(unsigned peer=1;peer<kMaximumPlayers;++peer) {
        if(!g_session.peers[peer].connected) continue;
        RaceSnapshotPacket packet{};
        std::array<unsigned,4> slots{};unsigned count=0;
        auto flush=[&] {
            if(!count) return;
            initialize_packet(packet,PacketType::RaceSnapshot);
            packet.round=g_session.game_setup.revision;
            packet.header.size=static_cast<std::uint16_t>(offsetof(RaceSnapshotPacket,riders)+count*sizeof(WireRiderState));
            const int sent=sendto(g_session.socket,reinterpret_cast<const char*>(&packet),packet.header.size,0,
                reinterpret_cast<const sockaddr*>(&g_session.peers[peer].endpoint),sizeof(sockaddr_in));
            if(sent==packet.header.size) for(unsigned i=0;i<count;++i) {
                const unsigned slot=slots[i];
                g_session.sent_rider_ticks[peer][slot]=g_session.riders[slot].tick;
                g_session.sent_rider_at[peer][slot]=now;
            }
            else if(sent==SOCKET_ERROR) record_socket_error_locked("rider-send",WSAGetLastError());
            // GCC needs an explicitly typed value for this nested packet aggregate.
#if defined(__GNUC__) && !defined(__clang__)
            packet=RaceSnapshotPacket{};
#else
            packet={};
#endif
            count=0;
        };
        for(unsigned slot=0;slot<kMaximumPlayers;++slot) {
            const auto &rider=g_session.riders[slot];
            if(slot==peer || !rider.active) continue;
            if(g_session.sent_rider_ticks[peer][slot]==rider.tick &&
               now-g_session.sent_rider_at[peer][slot]<kRiderRepairInterval) continue;
            slots[count]=slot;packet.slot_mask|=1u<<slot;
            packet.riders[count++]=to_wire(rider);
            if(count==4) flush();
        }
        flush();
    }
    if(g_session.world.round==g_session.game_setup.revision && g_session.world.tick &&
       now-g_session.last_world_snapshot>=kWorldSnapshotInterval) {
        for(unsigned b=0;b<world_sync::batches;++b) {
            WorldSnapshotPacket packet{};initialize_packet(packet,PacketType::WorldSnapshot);
            packet.batch.round=g_session.world.round;packet.batch.tick=g_session.world.tick;packet.batch.index=b;
            for(unsigned i=0;i<world_sync::batch_size && b*world_sync::batch_size+i<world_sync::capacity;++i)
                packet.batch.traffic[i]=g_session.world.traffic[b*world_sync::batch_size+i];
            for(unsigned slot=1;slot<kMaximumPlayers;++slot)
                if(g_session.peers[slot].connected) send_packet_locked(packet,g_session.peers[slot].endpoint);
        }
        advance_race_deadline(g_session.last_world_snapshot,now,kWorldSnapshotInterval);
    }
    advance_race_deadline(g_session.last_race_snapshot,now,kRaceSnapshotInterval);
}

void send_ping_locked(std::uint8_t slot, const sockaddr_in &endpoint, const Clock::time_point now) {
    PingPacket packet{};
    initialize_packet(packet, PacketType::Ping);
    packet.slot = slot;
    packet.nonce =
        g_session.sequence ^
        static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count());
    if (is_host_locked()) {
        g_session.peers[slot].ping_nonce = packet.nonce;
        g_session.peers[slot].ping_sent = now;
    } else {
        g_session.client_ping_nonce = packet.nonce;
        g_session.client_ping_sent = now;
    }
    send_packet_locked(packet, endpoint);
}

bool valid_packet(const PacketHeader &header, int received_size) {
    return received_size >= static_cast<int>(sizeof(PacketHeader)) &&
           header.magic == kProtocolMagic && header.version == kProtocolVersion &&
           header.size == received_size;
}
void send_reject_locked(const sockaddr_in &endpoint, RejectReason reason) {
    ConnectRejectPacket packet{};
    initialize_packet(packet, PacketType::ConnectReject);
    packet.header.session = 0;
    packet.reason = reason;
    send_packet_locked(packet, endpoint);
}

#include "rr64_hit_channel.inc"
#include "rr64_authoritative_channel.inc"

bool highlights_enabled_locked() {
    return g_session.authoritative && !g_session.host_disconnected && g_session.authority_finish_mode &&
        g_session.phase==Phase::Race && g_session.authority_round==g_session.game_setup.revision;
}
std::uint64_t highlight_time(const Clock::time_point now) {
    return std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count();
}
unsigned highlight_peers_locked() {
    unsigned mask=1;
    for(unsigned p=1;p<kMaximumPlayers;++p)
        if(g_session.peers[p].connected && (g_session.authority_humans&(1u<<p)))mask|=1u<<p;
    return mask;
}
bool handle_highlight_locked(const std::uint8_t* bytes,int size,unsigned sender,const Clock::time_point now) {
    if(reinterpret_cast<const PacketHeader*>(bytes)->type!=PacketType::Highlight)return false;
    if(!highlights_enabled_locked() || size<int(sizeof(PacketHeader)+5) ||
       size>int(sizeof(PacketHeader)+highlight_network::maximum_datagram))return true;
    if(is_host_locked() && !(g_session.authority_humans&(1u<<sender)))return true;
    const auto delay=is_client_locked() && g_session.local_slot<kMaximumPlayers ?
        std::uint64_t(g_session.players[g_session.local_slot].ping_ms)*500 : 0;
    g_session.highlights.receive(sender,{bytes+sizeof(PacketHeader),std::size_t(size)-sizeof(PacketHeader)},highlight_time(now),delay);
    return true;
}
void service_highlights_locked(const Clock::time_point now) {
    if(!highlights_enabled_locked())return;
    g_session.highlights.service(highlight_time(now),highlight_peers_locked(),[](unsigned peer,std::span<const std::uint8_t> payload){
        if(payload.size()>highlight_network::maximum_datagram || g_session.socket==INVALID_SOCKET)return;
        std::array<std::uint8_t,sizeof(PacketHeader)+highlight_network::maximum_datagram> bytes{};
        PacketHeader header{};header.type=PacketType::Highlight;header.size=std::uint16_t(sizeof(header)+payload.size());
        header.sequence=g_session.sequence++;header.session=g_session.token;
        std::memcpy(bytes.data(),&header,sizeof(header));std::memcpy(bytes.data()+sizeof(header),payload.data(),payload.size());
        const auto&endpoint=is_host_locked()?g_session.peers[peer].endpoint:g_session.host_endpoint;
        if(sendto(g_session.socket,reinterpret_cast<const char*>(bytes.data()),header.size,0,
                  reinterpret_cast<const sockaddr*>(&endpoint),sizeof(endpoint))==SOCKET_ERROR)
            record_socket_error_locked("highlight send",WSAGetLastError());
    });
}

void handle_host_packet_locked(const std::uint8_t *bytes, int size, const sockaddr_in &endpoint,
                               const Clock::time_point now) {
    const PacketHeader &header = *reinterpret_cast<const PacketHeader *>(bytes);
    if (header.type == PacketType::Hello && size == sizeof(HelloPacket)) {
        std::uint8_t slot = find_peer_slot_locked(endpoint);
        if (slot == kInvalidSlot) {
            if (g_session.phase != Phase::Lobby) {
                send_reject_locked(endpoint, RejectReason::Busy);
                return;
            }
            if (connected_count_locked() >=
                std::clamp<unsigned>(g_session.config.maximum_players, 2u, kMaximumPlayers)) {
                send_reject_locked(endpoint, RejectReason::Full);
                return;
            }
            slot = allocate_peer_slot_locked();
            if (slot == kInvalidSlot) {
                return;
            }
            g_session.peers[slot].connected = true;
            g_session.course_ack[slot] = 0;
            g_session.peers[slot].endpoint = endpoint;
            g_session.players[slot].connected = true;
            g_session.players[slot].slot = slot;
            g_session.players[slot].ready = false;
            if (rr64::diagnostics::network_detail_enabled()) {
                std::fprintf(stderr, "[RR64-NET] hello accepted slot=%u peer=%s:%u\n", slot,
                             inet_ntoa(endpoint.sin_addr), ntohs(endpoint.sin_port));
                std::fflush(stderr);
            }
        }
        g_session.peers[slot].last_seen = now;
        const auto &hello = *reinterpret_cast<const HelloPacket *>(bytes);
        g_session.players[slot].name = read_string(hello.player_name, sizeof(hello.player_name));
        if (g_session.players[slot].name.empty()) {
            g_session.players[slot].name = "Rider " + std::to_string(slot + 1);
        }
        send_welcome_locked(slot);
        send_snapshot_locked(now);
        g_session.message = "Lobby open - " + std::to_string(connected_count_locked()) + " / " +
                            std::to_string(kMaximumPlayers) + " riders";
        return;
    }

    if (header.session != g_session.token) {
        return;
    }
    const std::uint8_t slot = find_peer_slot_locked(endpoint);
    if (slot == kInvalidSlot) {
        return;
    }
    g_session.peers[slot].last_seen = now;

    // Gameplay messages must pass session and endpoint ownership checks above.
    if(handle_authority_locked(bytes,size,slot,endpoint)) return;
    if(handle_hit_locked(bytes,size,slot,endpoint,now)) return;
    if(handle_highlight_locked(bytes,size,slot,now)) return;

    if (header.type == PacketType::ClientState && size == sizeof(ClientStatePacket)) {
        const auto &state = *reinterpret_cast<const ClientStatePacket *>(bytes);
        if (state.slot != slot || state.course_compatibility >
            static_cast<std::uint8_t>(race_pack::Compatibility::Different)) {
            return;
        }
        // UDP can reorder client input packets. Never rewind controls/readiness
        // to an older packet from the same authenticated peer.
        auto &lastSequence=g_session.peers[slot].last_state_sequence;
        if(lastSequence!=0&&static_cast<std::int32_t>(header.sequence-lastSequence)<=0)return;
        lastSequence=header.sequence;
        if(online_flow::valid(state.postrace_ack) &&
           state.postrace_ack.round==g_session.postrace.round &&
           state.postrace_ack.stage<=g_session.postrace.stage &&
           online_flow::advances(state.postrace_ack,g_session.postrace_ack[slot]))
            g_session.postrace_ack[slot]=state.postrace_ack;
        PlayerInfo &player = g_session.players[slot];
        if(g_session.game_setup.valid && g_session.phase!=Phase::Race) {
            if(!state.course_revision || state.course_revision!=g_session.game_setup.revision ||
               state.course!=g_session.game_setup.course) return;
            player.course_compatibility=static_cast<race_pack::Compatibility>(state.course_compatibility);
            g_session.course_ack[slot]=player.course_compatibility==race_pack::Compatibility::Ready
                ? state.course_revision : 0;
            if(!g_session.course_ack[slot]) {
                player.selection={};
                player.ready=false;
            }
        }
        if (online_flow::valid(state.selection) &&
            state.selection.round == g_session.game_setup.revision &&
            g_session.game_setup.valid && g_session.course_ack[slot]==g_session.game_setup.revision) {
            // Once loading starts, a peer may acknowledge loading but cannot
            // silently replace its already committed rider or bike.
            if (g_session.phase == Phase::CharacterSelect ||
                (g_session.phase == Phase::TrackSelect &&
                 state.selection.rider == player.selection.rider &&
                 state.selection.bike == player.selection.bike && state.selection.confirmed))
                player.selection = state.selection;
        }
        player.ready = state.ready != 0 && (!g_session.game_setup.valid ||
            g_session.course_ack[slot]==g_session.game_setup.revision);
        player.character = player.selection.round==g_session.game_setup.revision && player.selection.confirmed
            ? static_cast<std::uint8_t>(player.selection.rider) : state.character;
        player.track = state.track;
        player.name = read_string(state.player_name, sizeof(state.player_name));
        g_session.inputs[slot].buttons = g_session.phase == Phase::Race
            ? online_flow::race_buttons(state.buttons,slot) : state.buttons;
        g_session.inputs[slot].stick_x = static_cast<float>(state.stick_x) / 127.0f;
        g_session.inputs[slot].stick_y = static_cast<float>(state.stick_y) / 127.0f;
    } else if (header.type == PacketType::ClientRiderState &&
               size == sizeof(ClientRiderStatePacket)) {
        // In authority mode clients submit commands, never authoritative poses.
        if(g_session.authoritative) return;
        const auto &packet = *reinterpret_cast<const ClientRiderStatePacket *>(bytes);
        if (packet.slot != slot || packet.round != g_session.game_setup.revision || g_session.phase != Phase::Race) {
            return;
        }
        RiderState proposed = from_wire(packet.state);
        if (proposed.host_ai || !proposed.active || !finite_rider_state(proposed) ||
            proposed.tick <= g_session.riders[slot].tick) {
            return;
        }
        // The host owns the canonical array and accepts only the state carried
        // by the authenticated endpoint assigned to this slot. Game-level
        // sanity checks can be layered here without changing the wire format.
        g_session.previous_riders[slot] = g_session.riders[slot];
        g_session.riders[slot] = proposed;
    } else if (header.type == PacketType::Voice &&
               size >= static_cast<int>(offsetof(VoicePacket, payload))) {
        const auto &incoming = *reinterpret_cast<const VoicePacket *>(bytes);
        Peer &peer = g_session.peers[slot];
        if (peer.voice_window_start.time_since_epoch().count() == 0 ||
            now - peer.voice_window_start >= std::chrono::seconds(1)) {
            peer.voice_window_start = now;
            peer.voice_packets_in_window = 0;
        }
        if (g_session.phase != Phase::Race || incoming.speaker_slot != slot ||
            !valid_voice_packet(incoming, size) ||
            incoming.voice_sequence <= g_session.last_voice_sequences[slot] ||
            peer.voice_packets_in_window >= kMaximumVoicePacketsPerSecond) {
            return;
        }

        ++peer.voice_packets_in_window;
        g_session.last_voice_sequences[slot] = incoming.voice_sequence;
        VoicePacket canonical = incoming;
        initialize_packet(canonical, PacketType::Voice);
        canonical.speaker_slot = slot;
        for (std::uint8_t destination = 1; destination < kMaximumPlayers; ++destination) {
            if (destination != slot && g_session.peers[destination].connected) {
                send_voice_packet_locked(canonical, g_session.peers[destination].endpoint);
            }
        }
        enqueue_voice_frame_locked(canonical);
    } else if (header.type == PacketType::Ping && size == sizeof(PingPacket)) {
        PingPacket pong = *reinterpret_cast<const PingPacket *>(bytes);
        pong.header.type = PacketType::Pong;
        pong.header.sequence = g_session.sequence++;
        send_packet_locked(pong, endpoint);
    } else if (header.type == PacketType::Pong && size == sizeof(PingPacket)) {
        const auto &pong = *reinterpret_cast<const PingPacket *>(bytes);
        Peer &peer = g_session.peers[slot];
        if (pong.nonce == peer.ping_nonce) {
            const auto elapsed =
                std::chrono::duration_cast<std::chrono::milliseconds>(now - peer.ping_sent).count();
            g_session.players[slot].ping_ms =
                static_cast<std::uint16_t>(std::clamp<std::int64_t>(elapsed, 0, 999));
        }
    } else if (header.type == PacketType::Disconnect) {
        if(g_session.authoritative && (g_session.authority_humans&(1u<<slot))) {
            g_session.authority_host.disconnect(slot);
            g_session.authority_loaded|=1u<<slot;
            if (rr64::diagnostics::network_detail_enabled())
                std::fprintf(stderr,"[RR64-NET] peer %u left; race continues with neutral input.\n",unsigned(slot));
        }
        g_session.peers[slot] = {};
        g_session.course_ack[slot] = 0;
        g_session.players[slot] = {};
        g_session.inputs[slot] = {};
        g_session.riders[slot] = {};
        g_session.previous_riders[slot] = {};
    }
}

void handle_client_packet_locked(const std::uint8_t *bytes, int size, const sockaddr_in &endpoint,
                                 const Clock::time_point now) {
    if (!endpoint_equal(endpoint, g_session.host_endpoint)) {
        return;
    }
    const PacketHeader &header = *reinterpret_cast<const PacketHeader *>(bytes);
    if (g_session.phase == Phase::Connecting && header.type == PacketType::ConnectReject &&
        size == sizeof(ConnectRejectPacket)) {
        const auto &rejection = *reinterpret_cast<const ConnectRejectPacket *>(bytes);
        switch (rejection.reason) {
        case RejectReason::Full:
            connection_failure_locked("The host's lobby is full.");
            break;
        case RejectReason::Busy:
            connection_failure_locked(
                "The host has already started. Return both games to the lobby.");
            break;
        case RejectReason::Version:
            connection_failure_locked("Online versions differ. Run the same build on both PCs.");
            break;
        default:
            break;
        }
        return;
    }
    if (header.type == PacketType::Welcome && size == sizeof(WelcomePacket)) {
        if (g_session.phase != Phase::Connecting || g_session.local_slot != kInvalidSlot)
            return;
        const auto &welcome = *reinterpret_cast<const WelcomePacket *>(bytes);
        if (welcome.assigned_slot == 0 || welcome.assigned_slot >= kMaximumPlayers ||
            welcome.maximum_players != kMaximumPlayers || header.session == 0 ||
            welcome.phase != static_cast<std::uint8_t>(Phase::Lobby)) {
            return;
        }
        g_session.token = header.session;
        g_session.local_slot = welcome.assigned_slot;
        g_session.phase = static_cast<Phase>(welcome.phase);
        g_session.replicated_riders = welcome.replicated_riders != 0;
        PlayerInfo &local = g_session.players[g_session.local_slot];
        local.connected = true;
        local.slot = g_session.local_slot;
        local.name = g_session.config.player_name;
        g_session.message = "Connected to host";
        g_session.last_host_seen = now;
        if (rr64::diagnostics::network_detail_enabled()) {
            std::fprintf(stderr, "[RR64-NET] welcome accepted slot=%u attempts=%llu\n",
                         g_session.local_slot,
                         static_cast<unsigned long long>(g_session.hello_attempts));
            std::fflush(stderr);
        }
        send_client_state_locked(now);
        return;
    }
    if (g_session.token == 0 || header.session != g_session.token) {
        return;
    }
    g_session.last_host_seen = now;
    if(handle_authority_locked(bytes,size,0,endpoint)) return;
    if(handle_hit_locked(bytes,size,0,endpoint,now)) return;
    if(handle_highlight_locked(bytes,size,0,now)) return;

    if (header.type == PacketType::LobbySnapshot && size == sizeof(LobbySnapshotPacket)) {
        const auto &snapshot = *reinterpret_cast<const LobbySnapshotPacket *>(bytes);
        if(!online_flow::advances(snapshot.postrace,g_session.postrace)) return;
        if (snapshot.game_setup.start_requested > 1 || !race_pack::valid(snapshot.game_setup.course)) return;
        // A revision identifies immutable course bytes, including snapshots
        // received after local validation. A new course needs a new revision.
        if(g_session.game_setup.valid && snapshot.game_setup.revision==g_session.game_setup.revision &&
           snapshot.game_setup.course!=g_session.game_setup.course)return;
        for (const auto &player : snapshot.players)
            if (!online_flow::valid(player.selection) || player.course_compatibility >
                static_cast<std::uint8_t>(race_pack::Compatibility::Different)) return;
        if(!local_race_options::valid_online_options(snapshot.game_setup.race_options))return;
        if (snapshot.phase > static_cast<std::uint8_t>(Phase::Race)) {
            return;
        }
        if (header.sequence <= g_session.last_lobby_snapshot_sequence) {
            return;
        }
        g_session.last_lobby_snapshot_sequence = header.sequence;
        g_session.postrace = snapshot.postrace;
        const Phase incoming_phase = static_cast<Phase>(snapshot.phase);
        const bool preserve_local_ready = incoming_phase==Phase::Lobby && g_session.phase==Phase::Lobby;
        if (incoming_phase != g_session.phase) {
            g_session.received_voice_frames.clear();
            g_session.last_voice_sequences = {};
            g_session.local_voice_sequence = 0;
        }
        g_session.phase = incoming_phase;
        if(incoming_phase!=Phase::Race){g_session.authoritative=false;g_session.authority_loaded=0;g_session.authority_released=false;}
        g_session.replicated_riders = snapshot.replicated_riders != 0;
        g_session.config.maximum_players =
            static_cast<std::uint8_t>(std::clamp<unsigned>(snapshot.reserved, 2u, kMaximumPlayers));
        const bool setup_changed=snapshot.game_setup.valid!=g_session.game_setup.valid ||
            snapshot.game_setup.revision!=g_session.game_setup.revision ||
            snapshot.game_setup.course!=g_session.game_setup.course;
        if(setup_changed) {
            g_session.course_ack={};
            for(auto& player:g_session.players) player.course_compatibility=race_pack::Compatibility::Pending;
            g_session.riders={}; g_session.previous_riders={}; g_session.race_player_mask=0;
            g_session.world={};g_session.world_receiver.reset();
            for(unsigned slot=0;slot<kMaximumPlayers;++slot)
                if(snapshot.players[slot].connected) g_session.race_player_mask|=1u<<slot;
        }
        g_session.game_setup.valid = snapshot.game_setup.valid != 0;
        g_session.game_setup.revision = snapshot.game_setup.revision;
        g_session.game_setup.transition_buttons = snapshot.game_setup.transition_buttons;
        g_session.game_setup.race_options = snapshot.game_setup.race_options;
        g_session.game_setup.random_seed = snapshot.game_setup.random_seed;
        g_session.game_setup.course = snapshot.game_setup.course;
        g_session.game_setup.start_requested = snapshot.game_setup.start_requested;
        std::copy(std::begin(snapshot.game_setup.words), std::end(snapshot.game_setup.words),
                  g_session.game_setup.words.begin());
        for (std::uint8_t slot = 0; slot < kMaximumPlayers; ++slot) {
            const WirePlayer &wire = snapshot.players[slot];
            PlayerInfo &player = g_session.players[slot];
            // A delayed host echo cannot undo our newer local validation or
            // revocation. New revisions always start locally unvalidated.
            if(slot!=g_session.local_slot)
                player.course_compatibility=static_cast<race_pack::Compatibility>(wire.course_compatibility);
            if (slot != g_session.local_slot || g_session.phase != Phase::CharacterSelect)
                player.selection = wire.selection;
            player.connected = wire.connected != 0;
            // Readiness is locally owned while staying in the lobby. A delayed
            // host echo must not undo a toggle and resend the old value forever.
            if (slot!=g_session.local_slot || !preserve_local_ready)
                player.ready = wire.ready != 0;
            player.slot = player.connected ? slot : kInvalidSlot;
            player.character = wire.character;
            player.track = wire.track;
            player.ping_ms = wire.ping_ms;
            player.name = read_string(wire.name, sizeof(wire.name));
            // Host echo is delayed. Preserve our most recent locally sampled
            // controls so the next outgoing packet cannot resend stale input.
            if(slot!=g_session.local_slot){
                g_session.inputs[slot].buttons = wire.buttons;
                g_session.inputs[slot].stick_x = static_cast<float>(wire.stick_x) / 127.0f;
                g_session.inputs[slot].stick_y = static_cast<float>(wire.stick_y) / 127.0f;
            }
        }
        g_session.message = "Connected - " + std::to_string(snapshot.connected_players) + " / " +
                            std::to_string(kMaximumPlayers) + " riders";
    } else if (header.type == PacketType::WorldSnapshot && size == sizeof(WorldSnapshotPacket)) {
        if(g_session.authoritative || g_session.phase!=Phase::Race) return;
        const auto &packet=*reinterpret_cast<const WorldSnapshotPacket *>(bytes);
        if(g_session.world_receiver.accept(packet.batch,g_session.game_setup.revision))
            g_session.world=g_session.world_receiver.snapshot();
    } else if (header.type == PacketType::RaceSnapshot) {
        if(g_session.authoritative)return;
        if (g_session.phase != Phase::Race) return;
        if(size<int(offsetof(RaceSnapshotPacket,riders)+sizeof(WireRiderState)) || size>int(sizeof(RaceSnapshotPacket))) return;
        RaceSnapshotPacket snapshot{};std::memcpy(&snapshot,bytes,size);
        if (snapshot.round!=g_session.game_setup.revision) return;
        if(snapshot.slot_mask & ~((1u<<kMaximumPlayers)-1)) return;
        const unsigned count=std::popcount(snapshot.slot_mask);
        if(!count || count>4 || size!=int(offsetof(RaceSnapshotPacket,riders)+count*sizeof(WireRiderState))) return;
        std::array<unsigned,4> slots{};unsigned next=0;
        for(unsigned slot=0;slot<kMaximumPlayers;++slot) if(snapshot.slot_mask&(1u<<slot)) slots[next++]=slot;
        std::array<RiderState,4> incoming{};
        for (unsigned i=0;i<count;++i) {
            incoming[i]=from_wire(snapshot.riders[i]);
            const auto slot=slots[i];
            if (incoming[i].active && (!finite_rider_state(incoming[i]) ||
                (incoming[i].host_ai && (g_session.race_player_mask & (1u<<slot))))) return;
        }
        // Order by each actor's tick, not a global packet sequence: a later batch
        // may arrive before batch 0. Never replace our locally authored state.
        for (unsigned i=0;i<count;++i) {
            const unsigned slot=slots[i];
            if (slot!=g_session.local_slot && incoming[i].active &&
                incoming[i].tick>g_session.riders[slot].tick) {
                g_session.previous_riders[slot]=g_session.riders[slot];
                g_session.riders[slot]=incoming[i];
            }
        }
    } else if (header.type == PacketType::Voice &&
               size >= static_cast<int>(offsetof(VoicePacket, payload))) {
        const auto &packet = *reinterpret_cast<const VoicePacket *>(bytes);
        if (g_session.phase != Phase::Race || !valid_voice_packet(packet, size) ||
            packet.speaker_slot >= kMaximumPlayers || packet.speaker_slot == g_session.local_slot ||
            packet.voice_sequence <= g_session.last_voice_sequences[packet.speaker_slot]) {
            return;
        }
        g_session.last_voice_sequences[packet.speaker_slot] = packet.voice_sequence;
        enqueue_voice_frame_locked(packet);
    } else if (header.type == PacketType::Ping && size == sizeof(PingPacket)) {
        PingPacket pong = *reinterpret_cast<const PingPacket *>(bytes);
        pong.header.type = PacketType::Pong;
        pong.header.sequence = g_session.sequence++;
        send_packet_locked(pong, g_session.host_endpoint);
    } else if (header.type == PacketType::Pong && size == sizeof(PingPacket)) {
        const auto &pong = *reinterpret_cast<const PingPacket *>(bytes);
        if (pong.nonce == g_session.client_ping_nonce && g_session.local_slot < kMaximumPlayers) {
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     now - g_session.client_ping_sent)
                                     .count();
            g_session.players[g_session.local_slot].ping_ms =
                static_cast<std::uint16_t>(std::clamp<std::int64_t>(elapsed, 0, 999));
        }
    } else if (header.type == PacketType::Disconnect) {
        host_disconnected_locked();
    }
}

void pump_receive_locked(const Clock::time_point now) {
    if (g_session.socket == INVALID_SOCKET) {
        return;
    }
    std::array<std::uint8_t, 2048> buffer{};
    for (unsigned packets = 0; packets < 256; ++packets) {
        sockaddr_in endpoint{};
        socklen_type endpoint_size = sizeof(endpoint);
        const int received = recvfrom(g_session.socket, reinterpret_cast<char *>(buffer.data()),
                                      static_cast<int>(buffer.size()), 0,
                                      reinterpret_cast<sockaddr *>(&endpoint), &endpoint_size);
        if (received == SOCKET_ERROR) {
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK) {
                record_socket_error_locked("receive", error);
                g_session.message = "UDP receive error " + std::to_string(error);
            }
            break;
        }
        if (received < static_cast<int>(sizeof(PacketHeader))) {
            continue;
        }
        const auto &header = *reinterpret_cast<const PacketHeader *>(buffer.data());
        ++g_session.received_packets;
        if (header.magic == kProtocolMagic && header.size == received &&
            header.version != kProtocolVersion) {
            if (is_host_locked() && header.type == PacketType::Hello &&
                received == sizeof(HelloPacket))
                send_reject_locked(endpoint, RejectReason::Version);
            else if (is_client_locked() && g_session.phase == Phase::Connecting &&
                     endpoint_equal(endpoint, g_session.host_endpoint)) {
                connection_failure_locked(
                    "Online versions differ. Run the same build on both PCs.");
                return;
            }
            continue;
        }
        if (!valid_packet(header, received)) {
            continue;
        }
        if (is_host_locked()) {
            handle_host_packet_locked(buffer.data(), received, endpoint, now);
        } else if (is_client_locked()) {
            handle_client_packet_locked(buffer.data(), received, endpoint, now);
        }
        if (g_session.socket == INVALID_SOCKET)
            return;
    }
}

void expire_peers_locked(const Clock::time_point now) {
    if (!is_host_locked()) {
        return;
    }
    for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
        if (g_session.peers[slot].connected &&
            now - g_session.peers[slot].last_seen > kPeerTimeout) {
            if(g_session.authoritative && (g_session.authority_humans&(1u<<slot))) {
                g_session.authority_host.disconnect(slot);
                g_session.authority_loaded|=1u<<slot;
                std::fprintf(stderr,"[RR64-NET] peer %u timed out; race continues.\n",unsigned(slot));
            }
            g_session.peers[slot] = {};
            g_session.course_ack[slot] = 0;
            g_session.players[slot] = {};
            g_session.inputs[slot] = {};
            g_session.riders[slot] = {};
            g_session.previous_riders[slot] = {};
        }
    }
}

void service_locked(const Clock::time_point now) {
    service_authority_locked(now);
    service_hits_locked(now);
    if (g_session.config.mode == Mode::Offline || g_session.socket == INVALID_SOCKET) {
        return;
    }
    pump_receive_locked(now);
    if (g_session.socket == INVALID_SOCKET)
        return;
    expire_peers_locked(now);
    service_highlights_locked(now);

    if (is_client_locked()) {
        if (g_session.local_slot == kInvalidSlot &&
            now - g_session.connect_started >= kConnectTimeout) {
            connection_failure_locked(
                "Failed to connect: no hosted game responded. Ask the host to press Host first, then check the address and port and try again.");
            return;
        }
        if (g_session.local_slot != kInvalidSlot &&
            now - g_session.last_host_seen >= kPeerTimeout) {
            host_disconnected_locked();
            return;
        }
        if (g_session.local_slot == kInvalidSlot && now - g_session.last_hello >= kHelloInterval) {
            send_hello_locked(now);
        } else if (g_session.local_slot != kInvalidSlot &&
                   now - g_session.last_state >= kStateInterval) {
            send_client_state_locked(now);
        }
        if (g_session.local_slot != kInvalidSlot && g_session.phase == Phase::Race &&
            now - g_session.last_race_state >= kRaceStateInterval) {
            send_client_rider_state_locked(now);
        }
        if (g_session.local_slot != kInvalidSlot && now - g_session.last_ping >= kPingInterval) {
            send_ping_locked(g_session.local_slot, g_session.host_endpoint, now);
            g_session.last_ping = now;
        }
    } else {
        if (now - g_session.last_snapshot >= kSnapshotInterval) {
            send_snapshot_locked(now);
        }
        if (g_session.phase == Phase::Race &&
            now - g_session.last_race_snapshot >= kRaceSnapshotInterval) {
            send_race_snapshot_locked(now);
        }
        if (now - g_session.last_ping >= kPingInterval) {
            for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
                if (g_session.peers[slot].connected) {
                    send_ping_locked(slot, g_session.peers[slot].endpoint, now);
                }
            }
            g_session.last_ping = now;
        }
    }
}

} // namespace

bool publish_highlight_playlist(highlight_network::Blob blob,std::uint64_t duration_us) {
    if(prediction::active())return false;
    std::lock_guard lock(g_mutex);
    return highlights_enabled_locked() && is_host_locked() &&
        g_session.highlights.publish(std::move(blob),duration_us,highlight_time(Clock::now()));
}
std::optional<highlight_network::Blob> take_received_highlight_playlist() {
    if(prediction::active())return {};
    std::lock_guard lock(g_mutex);
    if(!highlights_enabled_locked() || !is_client_locked())return {};
    return g_session.highlights.take_received();
}
void acknowledge_highlight_decoded(bool accepted) {
    if(prediction::active())return;
    std::lock_guard lock(g_mutex);
    if(highlights_enabled_locked() && is_client_locked())g_session.highlights.decoded(accepted);
}
highlight_network::Status highlight_status() {
    if(prediction::active())return {};
    std::lock_guard lock(g_mutex);
    if(g_session.host_disconnected)return {highlight_network::Stage::Finished};
    if(!highlights_enabled_locked())return {};
    return g_session.highlights.status(highlight_time(Clock::now()),highlight_peers_locked());
}
bool host_begin_highlights() {
    if(prediction::active())return false;
    std::lock_guard lock(g_mutex);
    return highlights_enabled_locked() && is_host_locked() &&
        g_session.highlights.begin(highlight_time(Clock::now()),highlight_peers_locked());
}
bool host_skip_highlights() {
    if(prediction::active())return false;
    std::lock_guard lock(g_mutex);
    return highlights_enabled_locked() && is_host_locked() && g_session.highlights.skip();
}

bool authority_start() {
    std::lock_guard lock(g_mutex);
    if(!is_host_locked() || g_session.host_disconnected || g_session.phase!=Phase::Race || !g_session.game_setup.revision)return false;
    unsigned humans=g_session.race_player_mask;
    if(!humans || !(humans&1))return false;
    return enable_authority_locked(g_session.game_setup.revision,humans);
}
bool authority_race_gate(bool native_loaded) {
    std::lock_guard lock(g_mutex);
    if(!g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || g_session.local_slot>=kMaximumPlayers)return false;
    const auto local=1u<<g_session.local_slot;
    if(!(g_session.authority_humans&local))return false;
    if(native_loaded)g_session.authority_loaded|=local;
    if(is_host_locked() && g_session.authority_loaded==g_session.authority_humans)
        g_session.authority_released=true;
    return g_session.authority_released && (g_session.authority_loaded&local);
}
bool authority_set_finish_mode(unsigned mode) {
    std::lock_guard lock(g_mutex);
    if(!is_host_locked() || !g_session.authoritative || g_session.host_disconnected ||
       g_session.phase!=Phase::Race || !valid_finish_mode(mode) || !mode)return false;
    if(g_session.authority_finish_mode)return g_session.authority_finish_mode==mode;
    g_session.authority_finish_mode=mode;
    g_session.authority_finish_tick=g_session.authority_frame.stamp.tick;return true;
}
unsigned authority_finish_mode() {
    std::lock_guard lock(g_mutex);
    // Receiving the final packet is not applying it. Client teardown must wait
    // for successful native reconciliation, including an older replay that was
    // already in flight when a newer final snapshot arrived.
    const auto applied=is_host_locked()?g_session.authority_frame.stamp.tick:
        g_session.authority_client.reconciled_tick();
    return g_session.authoritative && !g_session.host_disconnected &&
        g_session.phase==Phase::Race &&
        g_session.authority_round==g_session.game_setup.revision &&
        applied>=g_session.authority_finish_tick ? g_session.authority_finish_mode : 0;
}
bool authority_queue_input(std::uint16_t buttons,std::int8_t x,std::int8_t y) {
    authority::Command accepted{};return authority_queue_input_recorded(buttons,x,y,accepted);
}
bool authority_queue_input_recorded(std::uint16_t buttons,std::int8_t x,std::int8_t y,authority::Command &accepted,std::uint8_t actions,unsigned duration_us) {
    std::lock_guard lock(g_mutex);
    const auto rejected=[&](const char *reason){
        if (rr64::diagnostics::network_detail_enabled()) {
            std::fprintf(stderr,"[RR64-NET] input rejected reason=%s slot=%u round=%u setup=%u released=%u loaded=%x humans=%x sequence=%u buttons=%04x stick=%d,%d actions=%u\n",
                reason,unsigned(g_session.local_slot),g_session.authority_round,g_session.game_setup.revision,
                g_session.authority_released,g_session.authority_loaded,g_session.authority_humans,
                g_session.authority_local_sequence,buttons,int(x),int(y),unsigned(actions));
        }
        return false;
    };
    if(!g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || !g_session.authority_released ||
       g_session.local_slot>=kMaximumPlayers ||
       !(g_session.authority_loaded&(1u<<g_session.local_slot)))return rejected("race-not-released");
    if(is_host_locked()) {
        // The host does not predict itself or await a network acknowledgement.
        // Advance only after admission; a full queue must not create a gap.
        if(g_session.authority_local_sequence==UINT32_MAX)return rejected("sequence-exhausted");
        authority::InputBatch b{};b.round=g_session.authority_round;b.count=1;
        b.commands[0]={b.round,g_session.authority_local_sequence+1,buttons,x,y,actions,duration_us};
        if(!g_session.authority_host.receive(0,b))return rejected("host-command-invalid-or-full");
        ++g_session.authority_local_sequence;accepted=b.commands[0];return true;
    }
    authority::Command c;
    if(!g_session.authority_client.append(buttons,x,y,c,actions,duration_us))return rejected("client-command-invalid-or-full");
    accepted=c;return true;
}
bool authority_begin_step(authority::Step &step,unsigned duration_us) {
    std::lock_guard lock(g_mutex);
    return is_host_locked() && g_session.authoritative && g_session.authority_released && !g_session.host_disconnected && g_session.phase==Phase::Race &&
        g_session.authority_round==g_session.game_setup.revision && g_session.authority_host.begin(step,duration_us);
}
bool authority_finish_step(std::uint64_t tick,authority::Stamp &stamp) {
    std::lock_guard lock(g_mutex);
    if(!is_host_locked() || !g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || !g_session.authority_host.finish(tick))return false;
    stamp=g_session.authority_host.stamp();return true;
}

bool submit_hit(HitEvent event) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative || g_session.phase!=Phase::Race || g_session.host_disconnected) return false;
    reset_hits_locked();
    if(!valid_hit_locked(event,g_session.local_slot)) return false;
    if(is_host_locked()) return route_hit_locked(event);
    if(g_session.hits.proposals.size()>=64) return false;
    event.id=g_session.hits.next_request++;
    g_session.hits.proposals.push_back({event,{}});return true;
}
void authority_fail(const char *reason) {
    std::lock_guard lock(g_mutex);
    stop_authority_locked(reason);
}

bool authority_publish_frame(const AuthorityFrame &frame) {
    std::lock_guard lock(g_mutex);
    if(!is_host_locked() || !g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       frame.stamp.round!=g_session.game_setup.revision || !frame.stamp.tick ||
       frame.stamp.tick<=g_session.authority_frame.stamp.tick || !authority::valid_timing(frame.timing) ||
       frame.cop_mode>1 || !std::isfinite(frame.cop_win_age) || frame.cop_win_age < -1)return false;
    if(!valid_course_item_state(frame.course_items) || !valid_course_hazard_state(frame.course_hazards) ||
       !mk64_items::valid(frame.mk64_items) ||
       ((frame.course_items.count || frame.course_hazards.count || frame.mk64_items.enabled) &&
        g_session.game_setup.course.version==0))return false;
    if(!world_sync::valid(world_sync::Snapshot{frame.stamp.round,frame.stamp.tick,frame.traffic}))return false;
    const auto completed=g_session.authority_host.stamp();
    if(frame.stamp.tick!=completed.tick || frame.stamp.acknowledged!=completed.acknowledged ||
       frame.stamp.simulated_us!=completed.simulated_us)return false;
    for(const auto &outcome:frame.outcomes)if(!authority::valid_outcome(outcome))return false;
    for(unsigned s=0;s<kMaximumPlayers;++s)if(frame.riders[s].active &&
       (!finite_rider_state(frame.riders[s]) || !authority::valid_outcome(frame.outcomes[s]) || !authority::valid_dynamics(frame.dynamics[s]) ||
        (frame.riders[s].host_ai && (g_session.authority_humans&(1u<<s)))))return false;
    commit_authority_frame_locked(frame);return true;
}
void authority_pin_frame(){
    std::lock_guard lock(g_mutex);
    authority_read.enabled=false;
    if(!is_client_locked() || !g_session.authoritative || g_session.host_disconnected ||
       g_session.phase!=Phase::Race || g_session.authority_round!=g_session.game_setup.revision)return;
    authority_read.frame=g_session.authority_frame;authority_read.token=g_session.token;
    authority_read.round=g_session.authority_round;authority_read.enabled=true;
}
bool authority_get_frame(AuthorityFrame &frame) {
    std::lock_guard lock(g_mutex);
    if(!g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || !authority_read_frame_locked().stamp.tick)return false;
    frame=authority_read_frame_locked();return true;
}
bool authority_get_outcome(unsigned slot,authority::Outcome &out,bool &cop_mode){
    std::lock_guard lock(g_mutex);
    if(slot>=kMaximumPlayers || !g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || !authority_read_frame_locked().stamp.tick ||
       (!authority_read_frame_locked().riders[slot].active && !authority_read_frame_locked().outcomes[slot].valid))return false;
    out=authority_read_frame_locked().outcomes[slot];cop_mode=authority_read_frame_locked().cop_mode!=0;return true;
}

bool authority_prepare_replay(AuthorityReplayPlan &plan) {
    std::lock_guard lock(g_mutex);
    if(!is_client_locked() || !g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race ||
       g_session.authority_round!=g_session.game_setup.revision || g_session.local_slot>=kMaximumPlayers ||
       !authority_read_frame_locked().stamp.tick || g_authority_replay_ticket==UINT64_MAX)return false;
    AuthorityReplayPlan candidate{};candidate.frame=authority_read_frame_locked();
    const auto &stamp=candidate.frame.stamp;
    if(!g_session.authority_client.replay_commands(stamp.round,stamp.tick,
       stamp.acknowledged[g_session.local_slot],candidate.commands,candidate.count))return false;
    candidate.ticket=++g_authority_replay_ticket;
    g_session.authority_replay_stamp=stamp;
    g_session.authority_replay_last=g_session.authority_client.last_sequence();
    g_session.authority_replay_ticket=candidate.ticket;
    plan=candidate;return true;
}
bool authority_commit_replay(std::uint64_t ticket) {
    std::lock_guard lock(g_mutex);
    if(!ticket || ticket!=g_session.authority_replay_ticket || !is_client_locked() ||
       !g_session.authoritative || g_session.host_disconnected || g_session.phase!=Phase::Race || g_session.local_slot>=kMaximumPlayers ||
       g_session.authority_round!=g_session.game_setup.revision)return false;
    const auto &stamp=g_session.authority_replay_stamp;
    if(!g_session.authority_client.commit_replay(stamp.round,stamp.tick,
       stamp.acknowledged[g_session.local_slot],g_session.authority_replay_last))return false;
    g_session.authority_replay_ticket=0;return true;
}
bool take_hit(HitEvent &event) {
    std::lock_guard lock(g_mutex);reset_hits_locked();
    if(g_session.phase!=Phase::Race || g_session.host_disconnected || g_session.hits.delivered.empty()) return false;
    event=g_session.hits.delivered.front();g_session.hits.delivered.pop_front();return true;
}

void configure(const Config &requested) {
    std::lock_guard lock(g_mutex);
    close_socket_locked();
    g_session = {};
    g_session.config = requested;
    if (g_session.config.player_name.empty()) {
        g_session.config.player_name = "Rider";
    }
    if (g_session.config.player_name.size() >= kPlayerNameCapacity) {
        g_session.config.player_name.resize(kPlayerNameCapacity - 1);
    }

    if (requested.mode == Mode::Offline) {
        g_session.phase = Phase::Offline;
        g_session.message = "Offline";
        return;
    }
    if (requested.mode == Mode::Join) {
        std::string host, error;
        if (!parse_connection_address(requested.host_address, g_session.config.port, host, error)) {
            connection_failure_locked(error);
            return;
        }
        g_session.config.host_address = host;
    }
    if (!g_session.config.port) {
        connection_failure_locked("Enter a UDP port from 1 to 65535.");
        return;
    }

    const bool host = requested.mode == Mode::Host;
    if (!create_socket_locked(host, g_session.config.port)) {
        g_session.config.mode = Mode::Offline;
        g_session.phase = Phase::Offline;
        return;
    }

    const Clock::time_point now = Clock::now();
    g_session.connect_started = now;
    g_session.last_hello = now - kHelloInterval;
    g_session.last_state = now - kStateInterval;
    g_session.last_snapshot = now - kSnapshotInterval;
    g_session.last_race_state = now - kRaceStateInterval;
    g_session.last_race_snapshot = now - kRaceSnapshotInterval;
    g_session.last_world_snapshot = now - kWorldSnapshotInterval;
    g_session.last_ping = now - kPingInterval;

    if (host) {
        g_session.token = make_session_token();
        g_session.local_slot = 0;
        g_session.phase = Phase::Lobby;
        g_session.players[0].connected = true;
        g_session.players[0].slot = 0;
        g_session.players[0].name = g_session.config.player_name;
        g_session.message = "Lobby open on UDP port " + std::to_string(g_session.config.port);
    } else {
        if (!resolve_host(g_session.config.host_address, g_session.config.port,
                          g_session.host_endpoint)) {
            close_socket_locked();
            g_session.config.mode = Mode::Offline;
            g_session.phase = Phase::Offline;
            g_session.message = "Could not resolve host address";
            return;
        }
        g_session.phase = Phase::Connecting;
        g_session.message = "Connecting to " + g_session.config.host_address + ":" +
                            std::to_string(g_session.config.port);
        if (rr64::diagnostics::network_detail_enabled()) {
            std::fprintf(stderr, "[RR64-NET] destination=%s:%u protocol=%u\n",
                         inet_ntoa(g_session.host_endpoint.sin_addr),
                         ntohs(g_session.host_endpoint.sin_port), kProtocolVersion);
            std::fflush(stderr);
        }
    }
}

void shutdown() {
    std::lock_guard lock(g_mutex);
    if (g_session.socket != INVALID_SOCKET && g_session.token != 0) {
        PacketHeader packet{};
        packet.type = PacketType::Disconnect;
        packet.size = sizeof(packet);
        packet.sequence = g_session.sequence++;
        packet.session = g_session.token;
        if (is_host_locked()) {
            for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
                if (g_session.peers[slot].connected) {
                    send_packet_locked(packet, g_session.peers[slot].endpoint);
                }
            }
        } else if (is_client_locked()) {
            send_packet_locked(packet, g_session.host_endpoint);
        }
    }
    close_socket_locked();
    g_session = {};
}

void update() {
    std::lock_guard lock(g_mutex);
    service_locked(Clock::now());
}

PhysicsRules get_physics_rules() {
    if (prediction::active()) return {};
    std::lock_guard lock(g_mutex);
    PhysicsRules rules;
    rules.active = g_session.config.mode != Mode::Offline &&
        (g_session.socket != INVALID_SOCKET || g_session.host_disconnected);
    rules.connected = is_host_locked() || g_session.local_slot != kInvalidSlot;
    rules.is_host = is_host_locked();
    rules.replicated_riders = g_session.replicated_riders;
    rules.authoritative = g_session.authoritative &&
        g_session.authority_round == g_session.game_setup.revision;
    rules.authority_humans = rules.authoritative ? g_session.authority_humans : 0;
    rules.local_slot = g_session.local_slot;
    rules.phase = g_session.phase;
    return rules;
}

Status get_status() {
    // Isolated local replay uses its copied guest roster, never live transport
    // ownership. Network-facing gameplay hooks then cannot send/consume live
    // events or consult a newer session while reconstructing historical input.
    if(prediction::active())return {};
    std::lock_guard lock(g_mutex);
    Status status{};
    status.postrace = g_session.postrace;
    status.host_disconnected = g_session.host_disconnected;
    if (status.host_disconnected)
        status.host_disconnect_age_ms = static_cast<std::uint32_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now()-g_session.host_disconnected_at).count());
    status.maximum_players = g_session.config.maximum_players;
    status.active = g_session.config.mode != Mode::Offline &&
        (g_session.socket != INVALID_SOCKET || g_session.host_disconnected);
    status.connected = is_host_locked() || g_session.local_slot != kInvalidSlot;
    status.is_host = is_host_locked();
    status.replicated_riders = g_session.replicated_riders;
    status.authoritative=g_session.authoritative && g_session.authority_round==g_session.game_setup.revision;
    status.authority_humans=status.authoritative?g_session.authority_humans:0;
    status.local_slot = g_session.local_slot;
    status.connected_players = connected_count_locked();
    status.phase = g_session.phase;
    status.game_setup = g_session.game_setup;
    status.message = g_session.message;
    status.players = g_session.players;
    return status;
}

bool set_ready(bool ready) {
    std::lock_guard lock(g_mutex);
    if (g_session.local_slot >= kMaximumPlayers ||
        !g_session.players[g_session.local_slot].connected) {
        return false;
    }
    if(ready && g_session.game_setup.valid &&
       g_session.course_ack[g_session.local_slot]!=g_session.game_setup.revision) return false;
    g_session.players[g_session.local_slot].ready = ready;
    return true;
}

bool set_character(std::uint8_t character) {
    std::lock_guard lock(g_mutex);
    if (g_session.local_slot >= kMaximumPlayers ||
        !g_session.players[g_session.local_slot].connected) {
        return false;
    }
    g_session.players[g_session.local_slot].character = character;
    return true;
}

bool set_track(std::uint8_t track) {
    std::lock_guard lock(g_mutex);
    if (g_session.local_slot >= kMaximumPlayers ||
        !g_session.players[g_session.local_slot].connected) {
        return false;
    }
    g_session.players[g_session.local_slot].track = track;
    return true;
}

bool all_connected_players_ready() {
    std::lock_guard lock(g_mutex);
    const std::uint8_t count = connected_count_locked();
    return count > 0 &&
           std::all_of(g_session.players.begin(), g_session.players.end(),
                       [](const PlayerInfo &player) {
                           return !player.connected || (player.ready && (!g_session.game_setup.valid ||
                               (player.slot<kMaximumPlayers && g_session.course_ack[player.slot]==g_session.game_setup.revision)));
                       });
}

bool host_set_phase(Phase phase) {
    std::lock_guard lock(g_mutex);
    // Character/track/race transitions belong to the validated setup and
    // readiness gates below. A UI phase request must never skip those gates.
    if (!is_host_locked() || (phase != Phase::Lobby && phase != Phase::GameSetup)) {
        return false;
    }
    if (phase == Phase::Lobby) {
        g_session.replicated_riders = false;
        g_session.game_setup = {};
        g_session.course_ack = {};
    } else if (phase == Phase::GameSetup) {
        g_session.replicated_riders = connected_count_locked() > kMaximumLocalControllers;
        g_session.game_setup = {};
        g_session.course_ack = {};
    }
    if (phase != g_session.phase) {
        g_session.received_voice_frames.clear();
        g_session.last_voice_sequences = {};
        g_session.local_voice_sequence = 0;
    }
    g_session.phase = phase;
    if(phase!=Phase::Race){g_session.authoritative=false;g_session.authority_loaded=0;g_session.authority_released=false;}
    for (PlayerInfo &player : g_session.players) {
        player.course_compatibility=race_pack::Compatibility::Pending;
        if (player.connected) {
            player.ready = false;
        }
    }
    if (g_session.local_slot < kMaximumPlayers) {
        g_session.players[g_session.local_slot].ready = true;
    }
    send_snapshot_locked(Clock::now());
    return true;
}

bool host_advance_postrace(unsigned stage) {
    std::lock_guard lock(g_mutex);
    if(!is_host_locked() || g_session.host_disconnected || !g_session.authoritative ||
       g_session.phase!=Phase::Race || g_session.authority_finish_mode!=0x1e ||
       !g_session.game_setup.revision || stage<1 || stage>2) return false;
    const online_flow::PostRaceState next{g_session.game_setup.revision,stage};
    if(!online_flow::advances(next,g_session.postrace) ||
       (stage==2 && g_session.postrace!=online_flow::PostRaceState{next.round,1}))
        return next==g_session.postrace;
    if(g_session.postrace!=next && std::getenv("RR64_SYNC_LOG"))
        std::fprintf(stderr,"[RR64-FLOW] host postrace round=%u stage=%u\n",next.round,next.stage);
    g_session.postrace=next;
    send_snapshot_locked(Clock::now());
    return true;
}

bool acknowledge_postrace(online_flow::PostRaceState state) {
    std::lock_guard lock(g_mutex);
    if(g_session.local_slot>=kMaximumPlayers || !state.round ||
       state.round!=g_session.postrace.round || state.stage>g_session.postrace.stage ||
       !online_flow::advances(state,g_session.postrace_ack[g_session.local_slot])) return false;
    g_session.postrace_ack[g_session.local_slot]=state;
    return true;
}

bool host_resume_postrace_setup() {
    {
        std::lock_guard lock(g_mutex);
        if(!is_host_locked() || g_session.host_disconnected || g_session.phase!=Phase::Race ||
           g_session.postrace.stage!=2 || g_session.postrace.round!=g_session.game_setup.revision)
            return false;
        // Keep the finished authority/replay available until every participant
        // has completed native teardown and actually entered the setup menu.
        for(unsigned slot=0;slot<kMaximumPlayers;++slot)
            if((g_session.authority_humans&(1u<<slot)) && (slot==0 || g_session.peers[slot].connected) &&
               g_session.postrace_ack[slot]!=g_session.postrace) return false;
    }
    const bool resumed=host_set_phase(Phase::GameSetup);
    if(resumed && std::getenv("RR64_SYNC_LOG"))
        std::fprintf(stderr,"[RR64-FLOW] all peers entered next setup\n");
    return resumed;
}

bool host_commit_game_setup(const GameSetupState &setup) {
    std::lock_guard lock(g_mutex);
    if(!local_race_options::valid_online_options(setup.race_options) || !race_pack::valid(setup.course))return false;
    if (!is_host_locked() || g_session.phase != Phase::GameSetup) {
        return false;
    }

    g_session.riders={}; g_session.previous_riders={}; g_session.race_player_mask=0;
    g_session.sent_rider_ticks={};g_session.sent_rider_at={};
            g_session.world={};g_session.world_receiver.reset();
    for(unsigned slot=0;slot<kMaximumPlayers;++slot)
        if(g_session.players[slot].connected) g_session.race_player_mask|=1u<<slot;
    g_session.game_setup = setup;
    g_session.game_setup.valid = true;
    g_session.game_setup.start_requested = 0;
    // Remains monotonic when returning to setup within the same connection.
    // Otherwise delayed confirmations from the previous race could look current.
    g_session.game_setup.revision = ++g_session.setup_serial;
    g_session.course_ack={};
    g_session.course_ack[0]=g_session.game_setup.revision;
    for (auto &player : g_session.players) {
        player.selection = {};
        player.course_compatibility = player.slot==0 ? race_pack::Compatibility::Ready : race_pack::Compatibility::Pending;
    }
    g_session.phase = Phase::CharacterSelect;
    g_session.message = "Host settings locked - choose your rider and bike";
    send_snapshot_locked(Clock::now());
    return true;
}

bool acknowledge_course(std::uint32_t revision,const race_pack::Identity& course) {
    return report_course_compatibility(revision,course,race_pack::Compatibility::Ready);
}

const char* course_compatibility_message(race_pack::Compatibility compatibility) noexcept {
    switch(compatibility) {
    case race_pack::Compatibility::Missing: return "MK64 pack missing. Install the host's matching pack, or ask the host to choose a stock track.";
    case race_pack::Compatibility::Disabled: return "MK64 pack disabled. Restart the application, enable it in Mods, then rejoin; or ask the host to choose a stock track.";
    case race_pack::Compatibility::Different: return "MK64 pack differs from the host's. Use the same pack version, or ask the host to choose a stock track.";
    case race_pack::Compatibility::Ready: return "Track content verified.";
    default: return "Checking track content...";
    }
}

bool report_course_compatibility(std::uint32_t revision,const race_pack::Identity& course,
                                 race_pack::Compatibility compatibility) {
    std::lock_guard lock(g_mutex);
    if(!g_session.game_setup.valid || !revision || revision!=g_session.game_setup.revision ||
       course!=g_session.game_setup.course || g_session.local_slot>=kMaximumPlayers ||
       !g_session.players[g_session.local_slot].connected ||
       static_cast<unsigned>(compatibility)>static_cast<unsigned>(race_pack::Compatibility::Different))return false;
    // A running race owns immutable content. Late feedback cannot change or
    // newly validate it; an already accepted Ready report is idempotent.
    if(g_session.phase==Phase::Race) return compatibility==race_pack::Compatibility::Ready &&
        g_session.course_ack[g_session.local_slot]==revision;
    if(g_session.phase!=Phase::CharacterSelect && g_session.phase!=Phase::TrackSelect)return false;
    auto& player=g_session.players[g_session.local_slot];
    player.course_compatibility=compatibility;
    g_session.course_ack[g_session.local_slot]=compatibility==race_pack::Compatibility::Ready ? revision : 0;
    if(compatibility!=race_pack::Compatibility::Ready) {
        player.ready=false;
        player.selection={};
    }
    return true;
}

void reject_course_setup(const char *reason) {
    std::lock_guard lock(g_mutex);
    if(!is_client_locked() || g_session.authoritative || !g_session.game_setup.valid ||
       g_session.phase==Phase::Race || g_session.local_slot>=kMaximumPlayers)return;
    g_session.course_ack[g_session.local_slot]=0;
    auto& player=g_session.players[g_session.local_slot];
    player.course_compatibility=race_pack::Compatibility::Different;
    player.selection={};player.ready=false;
    g_session.message=reason?reason:course_compatibility_message(player.course_compatibility);
}

bool set_selection(const online_flow::Selection &selection) {
    std::lock_guard lock(g_mutex);
    if (!online_flow::valid(selection) || !g_session.game_setup.valid ||
        selection.round != g_session.game_setup.revision || g_session.local_slot >= kMaximumPlayers)
        return false;
    if(g_session.course_ack[g_session.local_slot]!=g_session.game_setup.revision)return false;
    auto &current = g_session.players[g_session.local_slot].selection;
    if (g_session.phase != Phase::CharacterSelect &&
        !(g_session.phase == Phase::TrackSelect && selection.confirmed &&
          selection.rider == current.rider && selection.bike == current.bike)) return false;
    current = selection;
    g_session.players[g_session.local_slot].character=static_cast<std::uint8_t>(selection.rider);
    return true;
}

bool host_request_race_start() {
    std::lock_guard lock(g_mutex);
    if (!is_host_locked() || g_session.phase != Phase::TrackSelect || !g_session.game_setup.valid) return false;
    for(const auto& player:g_session.players)
        if(player.connected && (player.slot>=kMaximumPlayers ||
            g_session.course_ack[player.slot]!=g_session.game_setup.revision)) return false;
    if (!g_session.game_setup.start_requested) {
        g_session.game_setup.start_requested=1;
        if (std::getenv("RR64_SYNC_LOG")) std::fprintf(stderr,"[RR64-FLOW] Host start requested round=%u\n",g_session.game_setup.revision);
        send_snapshot_locked(Clock::now());
    }
    return true;
}

bool host_release_selection() {
    std::lock_guard lock(g_mutex);
    if (!is_host_locked() || !g_session.game_setup.valid ||
        (g_session.phase != Phase::CharacterSelect && g_session.phase != Phase::TrackSelect)) return false;
    unsigned count = 0;
    for (const auto &p : g_session.players) {
        if (!p.connected) continue;
        ++count;
        if (p.slot>=kMaximumPlayers || g_session.course_ack[p.slot]!=g_session.game_setup.revision ||
            p.selection.round != g_session.game_setup.revision || !p.selection.confirmed ||
            (g_session.phase == Phase::TrackSelect && !p.selection.loaded)) return false;
    }
    if (count < 2) return false;
    if (g_session.phase == Phase::TrackSelect && !g_session.game_setup.start_requested) return false;
    g_session.phase = g_session.phase == Phase::CharacterSelect ? Phase::TrackSelect : Phase::Race;
    if (std::getenv("RR64_SYNC_LOG")) std::fprintf(stderr,"[RR64-FLOW] round=%u phase=%u peers=%u\n",g_session.game_setup.revision,unsigned(g_session.phase),count);
    send_snapshot_locked(Clock::now());
    return true;
}

void set_local_input(std::uint16_t buttons, float stick_x, float stick_y) {
    std::lock_guard lock(g_mutex);
    if (g_session.local_slot >= kMaximumPlayers) {
        return;
    }
    InputState &input = g_session.inputs[g_session.local_slot];
    input.buttons = g_session.phase == Phase::Race
        ? online_flow::race_buttons(buttons,g_session.local_slot) : buttons;
    input.stick_x = std::clamp(stick_x, -1.0f, 1.0f);
    input.stick_y = std::clamp(stick_y, -1.0f, 1.0f);
}

bool get_player_input(std::uint8_t slot, std::uint16_t &buttons, float &stick_x, float &stick_y) {
    std::lock_guard lock(g_mutex);
    if (slot >= kMaximumPlayers || !g_session.players[slot].connected) {
        buttons = 0;
        stick_x = 0.0f;
        stick_y = 0.0f;
        return false;
    }
    const InputState &input = g_session.inputs[slot];
    buttons = g_session.phase == Phase::Race ? online_flow::race_buttons(input.buttons,slot) : input.buttons;
    stick_x = input.stick_x;
    stick_y = input.stick_y;
    return true;
}

void set_local_rider_state(const RiderState &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative)return;
    if (g_session.local_slot >= kMaximumPlayers || !finite_rider_state(state)) {
        return;
    }
    RiderState canonical = state;
    canonical.active = true;
    canonical.host_ai = false;
    canonical.character = g_session.players[g_session.local_slot].character;
    if (canonical.tick <= g_session.riders[g_session.local_slot].tick) {
        canonical.tick = g_session.riders[g_session.local_slot].tick + 1;
    }
    g_session.previous_riders[g_session.local_slot] = g_session.riders[g_session.local_slot];
    g_session.riders[g_session.local_slot] = canonical;
}

// AI uses the unoccupied tail of the fixed race roster, never a controller slot.
bool set_host_world_state(const world_sync::Snapshot &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative || !is_host_locked() || g_session.phase!=Phase::Race ||
        state.round!=g_session.game_setup.revision || state.tick<=g_session.world.tick || !world_sync::valid(state)) return false;
    g_session.world=state;return true;
}
bool get_world_state(world_sync::Snapshot &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative){
        const auto &frame=authority_read_frame_locked();
        if(g_session.host_disconnected || g_session.phase!=Phase::Race || !frame.stamp.tick ||
           frame.stamp.round!=g_session.game_setup.revision)return false;
        state={frame.stamp.round,frame.stamp.tick,frame.traffic};return true;
    }
    if(g_session.phase!=Phase::Race || !g_session.world.tick || g_session.world.round!=g_session.game_setup.revision) return false;
    state=g_session.world;return true;
}

bool set_host_ai_rider_state(std::uint8_t slot, const RiderState &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative)return false;
    if (!is_host_locked() || g_session.phase!=Phase::Race || slot>=kMaximumPlayers ||
        (g_session.race_player_mask & (1u<<slot)) || !state.active || !finite_rider_state(state)) return false;
    auto next=state; next.host_ai=true;
    if(next.tick<=g_session.riders[slot].tick) next.tick=g_session.riders[slot].tick+1;
    g_session.previous_riders[slot]=g_session.riders[slot];
    g_session.riders[slot]=next;
    return true;
}

bool get_rider_state(std::uint8_t slot, RiderState &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative){
        const auto &frame=authority_read_frame_locked();
        if(g_session.host_disconnected || g_session.phase!=Phase::Race || slot>=kMaximumPlayers ||
           frame.stamp.round!=g_session.game_setup.revision || !frame.riders[slot].active){state={};return false;}
        state=frame.riders[slot];return true;
    }
    if (slot >= kMaximumPlayers || (!g_session.authoritative && !g_session.players[slot].connected && !g_session.riders[slot].host_ai) ||
        !g_session.riders[slot].active) {
        state = {};
        return false;
    }
    state = g_session.riders[slot];
    return true;
}

bool get_interpolated_rider_state(std::uint8_t slot, float alpha, RiderState &state) {
    std::lock_guard lock(g_mutex);
    if(g_session.authoritative){
        const auto &frame=authority_read_frame_locked();
        if(g_session.host_disconnected || g_session.phase!=Phase::Race || slot>=kMaximumPlayers ||
           frame.stamp.round!=g_session.game_setup.revision || !frame.riders[slot].active){state={};return false;}
        state=frame.riders[slot];return true;
    }
    if (slot >= kMaximumPlayers || (!g_session.authoritative && !g_session.players[slot].connected && !g_session.riders[slot].host_ai) ||
        !g_session.riders[slot].active) {
        state = {};
        return false;
    }
    state = interpolate(g_session.previous_riders[slot], g_session.riders[slot],
                        std::clamp(alpha, 0.0f, 1.0f));
    return true;
}

bool submit_local_voice(std::span<const std::uint8_t> encoded_frame) {
    std::lock_guard lock(g_mutex);
    if (g_session.socket == INVALID_SOCKET || g_session.phase != Phase::Race ||
        g_session.local_slot >= kMaximumPlayers || encoded_frame.empty() ||
        encoded_frame.size() > kVoicePayloadCapacity) {
        return false;
    }

    VoicePacket packet{};
    initialize_packet(packet, PacketType::Voice);
    packet.speaker_slot = g_session.local_slot;
    packet.payload_size = static_cast<std::uint16_t>(encoded_frame.size());
    packet.voice_sequence = ++g_session.local_voice_sequence;
    std::copy(encoded_frame.begin(), encoded_frame.end(), packet.payload);

    if (is_host_locked()) {
        for (std::uint8_t slot = 1; slot < kMaximumPlayers; ++slot) {
            if (g_session.peers[slot].connected) {
                send_voice_packet_locked(packet, g_session.peers[slot].endpoint);
            }
        }
    } else if (is_client_locked() && g_session.token != 0) {
        send_voice_packet_locked(packet, g_session.host_endpoint);
    } else {
        return false;
    }
    return true;
}

std::vector<ReceivedVoiceFrame> take_received_voice_frames() {
    std::lock_guard lock(g_mutex);
    std::vector<ReceivedVoiceFrame> frames;
    frames.reserve(g_session.received_voice_frames.size());
    while (!g_session.received_voice_frames.empty()) {
        frames.push_back(std::move(g_session.received_voice_frames.front()));
        g_session.received_voice_frames.pop_front();
    }
    return frames;
}

} // namespace rr64::netplay
