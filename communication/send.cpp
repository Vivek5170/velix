#include "socket_wrapper.hpp"

#include <cstdint>
#include <cstring>

#if __has_include(<nlohmann/json.hpp>)
    #include <nlohmann/json.hpp>
    inline constexpr bool kHasNlohmannJson = true;
#elif __has_include("../vendor/nlohmann/json.hpp")
    #include "../vendor/nlohmann/json.hpp"
    inline constexpr bool kHasNlohmannJson = true;
#else
    inline constexpr bool kHasNlohmannJson = false;
#endif

namespace velix::communication {

static void send_all(SocketWrapper& socket, const char* data, std::size_t len) {
    std::size_t total = 0;

    while (total < len) {
        auto sent = socket.send(data + total, static_cast<int>(len - total));
        if (sent <= 0) {
            throw SocketException("send failed");
        }
        total += static_cast<std::size_t>(sent);
    }
}

static void validate_json_if_available(const std::string& msg) {
#if __has_include(<nlohmann/json.hpp>) || __has_include("../vendor/nlohmann/json.hpp")
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

    send_all(socket, reinterpret_cast<const char*>(&net_len), sizeof(net_len));
    if (!json.empty()) {
        send_all(socket, json.data(), json.size());
    }
}

} // namespace velix::communication
