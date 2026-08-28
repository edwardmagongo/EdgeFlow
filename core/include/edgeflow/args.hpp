#pragma once
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace edgeflow {

// Extracts the value from a "--flag=value" argument. Throws
// std::invalid_argument if there is no '='.
inline std::string_view arg_value(std::string_view arg) {
    auto pos = arg.find('=');
    if (pos == std::string_view::npos) {
        throw std::invalid_argument("expected --flag=value, got: " + std::string(arg));
    }
    return arg.substr(pos + 1);
}

// Parses a non-negative integer flag value. Throws std::invalid_argument on
// empty input, a leading '-', non-numeric content, or trailing junk.
inline std::size_t parse_positive_size(std::string_view value, std::string_view flag_name) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    std::size_t pos = 0;
    unsigned long parsed;
    try {
        parsed = std::stoul(std::string(value), &pos);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (pos != value.size()) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    return static_cast<std::size_t>(parsed);
}

// Parses a "--port=value" style flag value, enforcing the 1-65535 TCP port
// range. Throws std::invalid_argument on invalid or out-of-range input.
inline std::uint16_t parse_port(std::string_view value) {
    std::size_t raw = parse_positive_size(value, "--port");
    if (raw == 0 || raw > 65535) {
        throw std::invalid_argument("--port must be between 1 and 65535, got: " + std::string(value));
    }
    return static_cast<std::uint16_t>(raw);
}

// Parses a percentage flag value (0-100 inclusive). Throws
// std::invalid_argument on invalid or out-of-range input.
inline double parse_percentage(std::string_view value, std::string_view flag_name) {
    if (value.empty() || value.front() == '-') {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    std::size_t pos = 0;
    double parsed;
    try {
        parsed = std::stod(std::string(value), &pos);
    } catch (const std::exception&) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (pos != value.size()) {
        throw std::invalid_argument("invalid value for " + std::string(flag_name) + ": " + std::string(value));
    }
    if (!std::isfinite(parsed) || parsed < 0.0 || parsed > 100.0) {
        throw std::invalid_argument(std::string(flag_name) + " must be between 0 and 100, got: " + std::string(value));
    }
    return parsed;
}

} // namespace edgeflow
