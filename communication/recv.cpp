#include "socket_wrapper.hpp"

#include <cstdint>

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

static void recv_all(SocketWrapper& socket, char* data, std::size_t len) {
    std::size_t total = 0;

    while (total < len) {
        auto rec = socket.recv(data + total, static_cast<int>(len - total));
        if (rec == 0) {
            throw SocketException("connection closed");
        }
        if (rec < 0) {
            throw SocketException("recv failed");
        }
        total += static_cast<std::size_t>(rec);
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

std::string recv_json(SocketWrapper& socket) {
    if (!socket.is_open()) {
        throw SocketException("socket not open");
    }

    std::uint32_t net_len = 0;
    recv_all(socket, reinterpret_cast<char*>(&net_len), sizeof(net_len));

    std::uint32_t len = ntohl(net_len);
    if (constexpr std::uint32_t kMaxMessageSize = 10U * 1024U * 1024U;
        len == 0 || len > kMaxMessageSize) {
        throw SocketException("invalid message size");
    }

    std::string msg(len, '\0');
    recv_all(socket, msg.data(), msg.size());
    validate_json_if_available(msg);
    return msg;
}

} // namespace velix::communication
