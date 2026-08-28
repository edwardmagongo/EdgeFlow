#pragma once
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

} // namespace edgeflow
