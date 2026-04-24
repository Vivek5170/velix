#pragma once

#include <string>
#include "socket_wrapper.hpp"
#include "json_include.hpp"

namespace velix::communication {

inline void validate_json_if_available(const std::string& msg) {
#if defined(VELIX_JSON_INCLUDED)
    try {
        [[maybe_unused]] auto parsed = nlohmann::json::parse(msg);
    } catch (const std::exception& e) {
        throw SocketException(std::string("Invalid JSON: ") + e.what());
    }
#else
    (void)msg;
#endif
}

} // namespace velix::communication
