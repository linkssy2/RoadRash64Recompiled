#pragma once
#include <charconv>
#include <string>
#include <string_view>
#include <cstdint>
namespace rr64::netplay {
inline std::string trim_address(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos)
        return {};
    return std::string(text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1));
}
inline bool parse_connection_address(std::string input, std::uint16_t &port, std::string &host,
                                     std::string &error) {
    host = trim_address(input);
    const auto colon = host.find(':');
    if (colon != std::string::npos) {
        if (host.find(':', colon + 1) != std::string::npos) {
            error = "Use an IPv4 address or hostname, not IPv6.";
            return false;
        }
        auto value = trim_address(host.substr(colon + 1));
        unsigned parsed = 0;
        auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed == 0 ||
            parsed > 65535) {
            error = "Enter a UDP port from 1 to 65535.";
            return false;
        }
        port = static_cast<std::uint16_t>(parsed);
        host = trim_address(host.substr(0, colon));
    }
    if (host.empty() || host.size() > 253 ||
        host.find_first_of(" /\\\t\r\n[]") != std::string::npos) {
        error = "Enter the host's IPv4 address or hostname.";
        return false;
    }
    if (!port) {
        error = "Enter a UDP port from 1 to 65535.";
        return false;
    }
    return true;
}
}
