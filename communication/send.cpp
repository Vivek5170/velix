#include "socket_wrapper.hpp"

#include <cstdint>
#include <cstring>

#if __has_include(<nlohmann/json.hpp>)
    #include <nlohmann/json.hpp>
    #define VELIX_HAS_NLOHMANN_JSON 1
#elif __has_include("../vendor/nlohmann/json.hpp")
    #include "../vendor/nlohmann/json.hpp"
    #define VELIX_HAS_NLOHMANN_JSON 1
#else
    #define VELIX_HAS_NLOHMANN_JSON 0
#endif

namespace velix::communication {

static void send_all(SocketWrapper& socket, const void* data, std::size_t len) {
    std::size_t total = 0;
    const char* ptr = static_cast<const char*>(data);

    while (total < len) {
        int sent = socket.send(ptr + total, static_cast<int>(len - total));
        if (sent <= 0) {
            throw SocketException("send failed");
        }
        total += static_cast<std::size_t>(sent);
    }
}

static void validate_json_if_available(const std::string& msg) {
#if VELIX_HAS_NLOHMANN_JSON
    try {
        [[maybe_unused]] auto parsed = nlohmann::json::parse(msg);
    } catch (const std::exception& e) {
        throw SocketException(std::string("Invalid JSON: ") + e.what());
    }
#else
    (void)msg;
#endif
}

void send_json(SocketWrapper& socket, const std::string& json) {
    if (!socket.is_open()) {
        throw SocketException("socket not open");
    }

    validate_json_if_available(json);

    std::uint32_t len = static_cast<std::uint32_t>(json.size());
    std::uint32_t net_len = htonl(len);

    send_all(socket, &net_len, sizeof(net_len));
    if (!json.empty()) {
        send_all(socket, json.data(), json.size());
    }
}

} // namespace velix::communication
