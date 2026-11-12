#pragma once
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace rr64::local_players {
inline std::atomic_bool active{false};
inline std::mutex names_mutex;
inline std::array<std::string, 4> names{"PLAYER 1", "PLAYER 2", "PLAYER 3", "PLAYER 4"};

// Stock display fields have twelve bytes including the terminator. Use the
// game's established uppercase Latin letters/digits rather than arbitrary UTF-8.
inline std::string display_name(std::string_view input, unsigned slot) {
    std::string result;
    for (unsigned char c : input) {
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 'A';
        if ((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ') {
            if (c == ' ' && result.empty())
                continue;
            result.push_back(static_cast<char>(c));
            if (result.size() == 11)
                break;
        }
    }
    while (!result.empty() && result.back() == ' ')
        result.pop_back();
    return result.empty() ? "PLAYER " + std::to_string(slot + 1) : result;
}
inline void set_name(unsigned slot, std::string_view input) {
    if (slot >= names.size())
        return;
    std::scoped_lock lock(names_mutex);
    names[slot] = display_name(input, slot);
}
inline std::array<std::string, 4> snapshot() {
    std::scoped_lock lock(names_mutex);
    return names;
}
}
