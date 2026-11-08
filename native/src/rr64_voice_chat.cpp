#include "rr64_voice_chat.hpp"
#include "rr64_diagnostic_options.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "opus.h"

#include "rr64_netplay.hpp"
#include "librecomp/config.hpp"
#include "recompui/config.h"
#include <string>

namespace rr64::voice_chat {
namespace {

constexpr int kVoiceRate = 48000;
constexpr int kFrameSamples = 960; // 20 ms at 48 kHz.
constexpr int kVoiceBitrate = 20000;
constexpr std::size_t kPlaybackPrebufferSamples = kFrameSamples * 3u;
constexpr std::size_t kMaximumPlaybackSamples = kFrameSamples * 12u;
constexpr float kVoiceMixGain = 0.72f;

struct PlaybackState {
    std::deque<std::int16_t> samples{};
    float gain = 0.0f;
    float target_gain = 0.0f;
    double phase = 0.0;
    std::int16_t current = 0;
    std::int16_t next = 0;
    bool primed = false;
    float pan = 0;
    float pitch = 1;
};

std::atomic_bool g_enabled = true;
std::atomic_bool g_muted=false;
std::atomic<float> g_input_gain=1.f,g_output_gain=1.f,g_threshold=0.01f;
std::atomic<float> g_flyby_strength=1.f;
struct MotionSample {float distance=0,pitch=1;std::uint64_t local_tick=0,remote_tick=0;
    std::chrono::steady_clock::time_point at{};bool detached=false,valid=false;};
std::array<MotionSample,netplay::kMaximumPlayers> g_motion{};
std::mutex g_settings_mutex;
std::string g_microphone, g_open_microphone;
std::vector<std::string> g_devices;
unsigned g_gate_hold=0;
// RR64-SDL3: capture runs through an audio stream bound to the recording
// device (SDL3 has no queued capture / SDL_DequeueAudio). A named microphone
// opens its physical device; System Default opens SDL's default recording
// device, which SDL3 moves to the new default by itself when it changes.
SDL_AudioStream* g_capture_stream = nullptr;
SDL_AudioDeviceID g_capture_physical_device = 0; // 0 for System Default
std::chrono::steady_clock::time_point g_next_presence_check{};
OpusEncoder *g_encoder = nullptr;
std::array<OpusDecoder *, netplay::kMaximumPlayers> g_decoders{};
std::array<PlaybackState, netplay::kMaximumPlayers> g_playback{};
std::mutex g_playback_mutex;
bool g_race_active = false;
bool g_capture_failure_reported = false;
std::chrono::steady_clock::time_point g_next_capture_attempt{};

void clear_playback() {
    std::lock_guard lock(g_playback_mutex);
    g_playback = {};
    g_motion = {};
}

void close_capture() {
    g_gate_hold=0;
    if (g_capture_stream != nullptr) {
        // Destroying a device stream also closes its logical device.
        SDL_DestroyAudioStream(g_capture_stream);
        g_capture_stream = nullptr;
    }
    g_capture_physical_device = 0;
}

// SDL2 reported a vanished capture device as SDL_AUDIO_STOPPED. SDL3 keeps a
// lost named device open as a silent "zombie", so check (at most once a
// second) that the physical microphone is still listed.
bool capture_device_present(std::chrono::steady_clock::time_point now) {
    if (g_capture_physical_device == 0 || now < g_next_presence_check) {
        return true;
    }
    g_next_presence_check = now + std::chrono::seconds(1);
    int count = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);
    bool present = false;
    for (int i = 0; devices != nullptr && i < count; ++i) {
        present = present || devices[i] == g_capture_physical_device;
    }
    SDL_free(devices);
    return present;
}

SDL_AudioDeviceID find_recording_device(const std::string &name) {
    int count = 0;
    SDL_AudioDeviceID *devices = SDL_GetAudioRecordingDevices(&count);
    SDL_AudioDeviceID found = 0;
    for (int i = 0; devices != nullptr && i < count && found == 0; ++i) {
        const char *device_name = SDL_GetAudioDeviceName(devices[i]);
        if (device_name != nullptr && name == device_name) {
            found = devices[i];
        }
    }
    SDL_free(devices);
    return found;
}

void reset_codecs() {
    if (g_encoder != nullptr) {
        opus_encoder_ctl(g_encoder, OPUS_RESET_STATE);
    }
    for (OpusDecoder *decoder : g_decoders) {
        if (decoder != nullptr) {
            opus_decoder_ctl(decoder, OPUS_RESET_STATE);
        }
    }
}

bool ensure_capture() {
    std::string selected;
    {std::lock_guard lock(g_settings_mutex);selected=g_microphone;}
    if(selected!=g_open_microphone){close_capture();g_open_microphone=selected;g_next_capture_attempt={};}
    const auto now=std::chrono::steady_clock::now();
    if(g_capture_stream && !capture_device_present(now))close_capture();
    if(g_capture_stream!=nullptr) return true;
    if(now<g_next_capture_attempt) return false;
    // Missing/busy devices should not trigger an expensive open every 5 ms.
    g_next_capture_attempt=now+std::chrono::seconds(1);
    if (g_encoder == nullptr) {
        int error = OPUS_OK;
        g_encoder = opus_encoder_create(kVoiceRate, 1, OPUS_APPLICATION_VOIP, &error);
        if (g_encoder == nullptr || error != OPUS_OK) {
            if (!g_capture_failure_reported) {
                std::fprintf(stderr, "[RR64-VOICE] Could not create Opus encoder (%d).\n", error);
                g_capture_failure_reported = true;
            }
            return false;
        }
        opus_encoder_ctl(g_encoder, OPUS_SET_BITRATE(kVoiceBitrate));
        opus_encoder_ctl(g_encoder, OPUS_SET_COMPLEXITY(5));
        opus_encoder_ctl(g_encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        opus_encoder_ctl(g_encoder, OPUS_SET_INBAND_FEC(1));
    }
    if (g_capture_stream != nullptr) {
        return true;
    }

    // The stream delivers 48 kHz mono S16 whatever the microphone's native
    // format is, the conversion SDL2's capture queue did.
    SDL_AudioSpec desired{};
    desired.freq = kVoiceRate;
    desired.format = SDL_AUDIO_S16;
    desired.channels = 1;
    SDL_AudioDeviceID device = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    if (!selected.empty()) {
        device = find_recording_device(selected);
        if (device == 0) {
            if (!g_capture_failure_reported) {
                std::fprintf(stderr, "[RR64-VOICE] Microphone unavailable: %s is not connected.\n", selected.c_str());
                g_capture_failure_reported = true;
            }
            return false;
        }
    }
    g_capture_stream = SDL_OpenAudioDeviceStream(device, &desired, nullptr, nullptr);
    if (g_capture_stream == nullptr) {
        if (!g_capture_failure_reported) {
            std::fprintf(stderr, "[RR64-VOICE] Microphone unavailable: %s\n", SDL_GetError());
            g_capture_failure_reported = true;
        }
        return false;
    }
    g_capture_physical_device = selected.empty() ? 0 : device;
    g_next_presence_check = now + std::chrono::seconds(1);
    g_capture_failure_reported = false;
    g_next_capture_attempt={};
    SDL_ResumeAudioStreamDevice(g_capture_stream);
    if (rr64::diagnostics::routine_enabled()) {
        std::fprintf(stderr, "[RR64-VOICE] Race microphone active at 48 kHz mono.\n");
    }
    return true;
}

OpusDecoder *decoder_for(std::uint8_t slot) {
    if (slot >= g_decoders.size()) {
        return nullptr;
    }
    OpusDecoder *&decoder = g_decoders[slot];
    if (decoder == nullptr) {
        int error = OPUS_OK;
        decoder = opus_decoder_create(kVoiceRate, 1, &error);
        if (error != OPUS_OK) {
            decoder = nullptr;
        }
    }
    return decoder;
}

void capture_frames() {
    if (!ensure_capture()) {
        return;
    }

    constexpr std::uint32_t frame_bytes = kFrameSamples * sizeof(std::int16_t);
    const auto available = [] {
        const int bytes = SDL_GetAudioStreamAvailable(g_capture_stream);
        return bytes > 0 ? static_cast<std::uint32_t>(bytes) : 0u;
    };
    if (available() > frame_bytes * 10u) {
        // A suspended or overloaded process must not resume by transmitting
        // stale speech several hundred milliseconds late.
        SDL_ClearAudioStream(g_capture_stream);
        return;
    }

    std::array<std::int16_t, kFrameSamples> pcm{};
    std::array<std::uint8_t, netplay::kVoicePayloadCapacity> encoded{};
    while (available() >= frame_bytes) {
        if (SDL_GetAudioStreamData(g_capture_stream, pcm.data(), static_cast<int>(frame_bytes)) != static_cast<int>(frame_bytes)) {
            break;
        }
        const float gain=g_input_gain.load();
        if(gate_level(pcm,gain)>=g_threshold.load())g_gate_hold=12; // 240ms release avoids clipped syllables.
        else if(g_gate_hold) --g_gate_hold;
        else continue;
        for(auto &sample:pcm)sample=static_cast<std::int16_t>(std::clamp(std::lround(sample*gain),-32768l,32767l));
        const int encoded_size = opus_encode(g_encoder, pcm.data(), kFrameSamples, encoded.data(),
                                             static_cast<opus_int32>(encoded.size()));
        if (encoded_size > 0) {
            netplay::submit_local_voice(std::span<const std::uint8_t>(
                encoded.data(), static_cast<std::size_t>(encoded_size)));
        }
    }
}

void decode_received_frames() {
    std::vector<netplay::ReceivedVoiceFrame> frames = netplay::take_received_voice_frames();
    for (const netplay::ReceivedVoiceFrame &frame : frames) {
        if (frame.speaker_slot >= netplay::kMaximumPlayers || frame.payload_size == 0 ||
            frame.payload_size > frame.payload.size()) {
            continue;
        }
        OpusDecoder *decoder = decoder_for(frame.speaker_slot);
        if (decoder == nullptr) {
            continue;
        }
        std::array<std::int16_t, kFrameSamples> decoded{};
        const int decoded_samples = opus_decode(decoder, frame.payload.data(), frame.payload_size,
                                                decoded.data(), kFrameSamples, 0);
        if (decoded_samples <= 0) {
            continue;
        }

        std::lock_guard lock(g_playback_mutex);
        PlaybackState &playback = g_playback[frame.speaker_slot];
        if (playback.target_gain <= 0.001f) {
            continue;
        }
        playback.samples.insert(playback.samples.end(), decoded.begin(),
                                decoded.begin() + decoded_samples);
        while (playback.samples.size() > kMaximumPlaybackSamples) {
            playback.samples.pop_front();
        }
    }
}

void update_spatial_gains(const netplay::Status &status) {
    netplay::RiderState local{};
    const bool has_local = status.local_slot < netplay::kMaximumPlayers &&
                           netplay::get_rider_state(status.local_slot, local);

    std::array<float, netplay::kMaximumPlayers> gains{};
    std::array<float, netplay::kMaximumPlayers> pans{};
    std::array<float, netplay::kMaximumPlayers> pitches;pitches.fill(1.f);
    const auto now=std::chrono::steady_clock::now();
    const auto has_body=[](const netplay::RiderState &r){
        return r.active && r.rider_position_valid && std::isfinite(r.rider_x) &&
               std::isfinite(r.rider_y) && std::isfinite(r.rider_z);
    };
    if (has_local && has_body(local)) {
        for (std::uint8_t slot = 0; slot < netplay::kMaximumPlayers; ++slot) {
            if (slot == status.local_slot || !status.players[slot].connected) {
                continue;
            }
            netplay::RiderState remote{};
            if (!netplay::get_rider_state(slot, remote) || !has_body(remote)) {
                continue;
            }
            // Speech and hearing originate at the bodies in every state.
            // Missing body data silences playback; never substitute bike position.
            const float dx = remote.rider_x-local.rider_x;
            const float dy = remote.rider_y-local.rider_y;
            const float dz = remote.rider_z-local.rider_z;
            const float distance=std::sqrt(dx * dx + dy * dy + dz * dz);
            gains[slot] = proximity_gain(distance);
            pans[slot]=direction_pan(dx,dz,local.front_wheel_x-local.rear_wheel_x,local.front_wheel_z-local.rear_wheel_z);
            auto &motion=g_motion[slot];
            const bool detached=remote.root.valid && !remote.root.rider_attached;
            if(motion.local_tick!=local.tick || motion.remote_tick!=remote.tick || !motion.valid){
                const float elapsed=std::chrono::duration<float>(now-motion.at).count();
                if(!motion.valid || detached!=motion.detached)motion.pitch=1; // No pitch impulse on eject/remount.
                else if(elapsed>=0.01f)motion.pitch=flyby_pitch(motion.distance,distance,elapsed,g_flyby_strength.load());
                if(!motion.valid || elapsed>=0.01f){
                    motion.distance=distance;motion.at=now;motion.valid=true;
                    motion.local_tick=local.tick;motion.remote_tick=remote.tick;motion.detached=detached;
                }
            }
            if(std::chrono::duration<float>(now-motion.at).count()<0.25f)pitches[slot]=motion.pitch;
        }
    }

    std::lock_guard lock(g_playback_mutex);
    for (std::size_t slot = 0; slot < g_playback.size(); ++slot) {
        g_playback[slot].target_gain = gains[slot];
        g_playback[slot].pan+=(pans[slot]-g_playback[slot].pan)*0.18f;
        g_playback[slot].pitch+=(pitches[slot]-g_playback[slot].pitch)*0.1f;
        if (gains[slot] <= 0.001f) {
            g_motion[slot]={};
            g_playback[slot].samples.clear();
            g_playback[slot].primed = false;
        }
        // Smooth rapid distance changes without keeping an out-of-range rider
        // audible for long after the separation occurs.
        g_playback[slot].gain += (gains[slot] - g_playback[slot].gain) * 0.18f;
    }
}

bool prime_playback(PlaybackState &playback) {
    if (playback.primed) {
        return true;
    }
    if (playback.samples.size() < kPlaybackPrebufferSamples) {
        return false;
    }
    playback.current = playback.samples.front();
    playback.samples.pop_front();
    playback.next = playback.samples.front();
    playback.samples.pop_front();
    playback.phase = 0.0;
    playback.primed = true;
    return true;
}

} // namespace

void apply_config(recomp::config::Config &config){
    g_muted.store(std::get<bool>(config.get_option_value("rr64_mic_mute")));
    g_input_gain.store(float(std::get<double>(config.get_option_value("rr64_mic_gain")))/100.f);
    g_output_gain.store(float(std::get<double>(config.get_option_value("rr64_voice_volume")))/100.f);
    g_flyby_strength.store(float(std::get<double>(config.get_option_value("rr64_voice_flyby")))/100.f);
    const auto db=std::get<double>(config.get_option_value("rr64_mic_threshold"));
    g_threshold.store(db<=-60?0.f:float(std::pow(10.,db/20.)));
    const auto selected=std::get<std::uint32_t>(config.get_option_value("rr64_microphone"));
    std::lock_guard lock(g_settings_mutex);
    g_microphone=selected<g_devices.size()?g_devices[selected]:std::string{};
}

void configure(recomp::config::Config &config){
    // Config enum keys persist the device name, not its changing SDL index.
    g_devices={""};
    std::vector<recomp::config::ConfigOptionEnumOption> choices{{0u,"system_default","System Default"}};
    // RR64-SDL3: recording devices are listed by ID; the saved key stays the name.
    int count=0;
    SDL_AudioDeviceID *devices=SDL_GetAudioRecordingDevices(&count);
    for(int i=0;devices!=nullptr && i<count;++i){
        const char *name=SDL_GetAudioDeviceName(devices[i]);
        if(!name || !*name || std::find(g_devices.begin(),g_devices.end(),name)!=g_devices.end())continue;
        const auto index=static_cast<unsigned>(g_devices.size());g_devices.emplace_back(name);
        choices.emplace_back(index,"device:"+g_devices.back(),g_devices.back());
    }
    SDL_free(devices);
    config.add_enum_option("rr64_microphone","Input Device",
        "Select your voice input. Connect microphones before opening the game; restart to refresh this list. System Default follows your operating system. If a saved device is unavailable at startup, select it again when reconnected.",choices,0u);
    config.add_bool_option("rr64_mic_mute","Mute Microphone","Stops microphone capture without muting other players.",false);
    config.add_number_option("rr64_mic_gain","Microphone Gain","Input level in percent. Lower this if your voice distorts.",0,200,5,0,false,100);
    config.add_number_option("rr64_mic_threshold","Voice Activation Threshold","Level in dB. Lower values pick up quieter speech; higher values reject more background noise. -60 keeps the microphone open during online races.",-60,-10,1,0,false,-40);
    config.add_number_option("rr64_voice_volume","Voice Chat Volume","Other players' voice volume in percent, separate from music and effects.",0,200,5,0,false,100);
    config.add_number_option("rr64_voice_flyby","Voice Fly-by Effect","Subtle pitch shift as riders approach or move away, including airborne crashes. 0 disables pitch changes; direction and distance still apply.",0,100,5,0,false,100);
    // Later tabs can relocate the frontend's config vector. Resolve the owned
    // config by its stable ID when loading settings or changing an option.
    for(const auto *key:{"rr64_microphone","rr64_mic_mute","rr64_mic_gain","rr64_mic_threshold","rr64_voice_volume","rr64_voice_flyby"})
        config.add_option_change_callback(key,[config_id = config.id](auto,auto,auto){
            apply_config(recompui::config::get_config(config_id));
        });
}

void set_enabled(bool enabled_value) {
    g_enabled.store(enabled_value, std::memory_order_release);
}

bool enabled() {
    return g_enabled.load(std::memory_order_acquire);
}

void update() {
    const netplay::Status status = netplay::get_status();
    const bool active =
        enabled() && status.active && status.connected && !status.host_disconnected && status.phase == netplay::Phase::Race;
    if (!active) {
        netplay::take_received_voice_frames();
        if (g_race_active) {
            close_capture();
            reset_codecs();
            clear_playback();
            g_race_active = false;
            if (rr64::diagnostics::routine_enabled()) {
                std::fprintf(stderr, "[RR64-VOICE] Race microphone stopped.\n");
            }
        }
        return;
    }

    g_race_active = true;
    if(g_muted.load())close_capture();else capture_frames();
    update_spatial_gains(status);
    decode_received_frames();
}

void mix(std::span<std::int16_t> stereo_samples, std::uint32_t output_rate) {
    if (!enabled() || output_rate == 0 || stereo_samples.size() < 2) {
        return;
    }

    const double source_step = static_cast<double>(kVoiceRate) / static_cast<double>(output_rate);
    std::lock_guard lock(g_playback_mutex);
    for (PlaybackState &playback : g_playback) {
        if (playback.gain <= 0.001f || !prime_playback(playback)) {
            continue;
        }
        const float volume=g_output_gain.load();
        const float left=std::sqrt((1.f-playback.pan)*0.5f)*volume;
        const float right=std::sqrt((1.f+playback.pan)*0.5f)*volume;
        for (std::size_t output = 0; output + 1 < stereo_samples.size(); output += 2) {
            const float interpolated =
                static_cast<float>(playback.current) +
                (static_cast<float>(playback.next) - static_cast<float>(playback.current)) *
                    static_cast<float>(playback.phase);
            const int voice =
                static_cast<int>(std::lround(interpolated * playback.gain * kVoiceMixGain));
            stereo_samples[output] = static_cast<std::int16_t>(
                std::clamp(static_cast<int>(stereo_samples[output]) + int(std::lround(voice*left)), -32768, 32767));
            stereo_samples[output + 1] = static_cast<std::int16_t>(
                std::clamp(static_cast<int>(stereo_samples[output + 1]) + int(std::lround(voice*right)), -32768, 32767));

            playback.phase += source_step*playback.pitch;
            while (playback.phase >= 1.0) {
                playback.phase -= 1.0;
                playback.current = playback.next;
                if (playback.samples.empty()) {
                    playback.primed = false;
                    break;
                }
                playback.next = playback.samples.front();
                playback.samples.pop_front();
            }
            if (!playback.primed) {
                break;
            }
        }
    }
}

void shutdown() {
    close_capture();
    if (g_encoder != nullptr) {
        opus_encoder_destroy(g_encoder);
        g_encoder = nullptr;
    }
    for (OpusDecoder *&decoder : g_decoders) {
        if (decoder != nullptr) {
            opus_decoder_destroy(decoder);
            decoder = nullptr;
        }
    }
    clear_playback();
    g_race_active = false;
}

} // namespace rr64::voice_chat
