#include "socket_wrapper.hpp"

#include <cstdint>

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

static void recv_all(SocketWrapper& socket, void* data, std::size_t len) {
    std::size_t total = 0;
    char* ptr = static_cast<char*>(data);

    while (total < len) {
        int rec = socket.recv(ptr + total, static_cast<int>(len - total));
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

std::string recv_json(SocketWrapper& socket) {
    if (!socket.is_open()) {
        throw SocketException("socket not open");
    }

    std::uint32_t net_len = 0;
    recv_all(socket, &net_len, sizeof(net_len));

    std::uint32_t len = ntohl(net_len);
    constexpr std::uint32_t MAX_MESSAGE_SIZE = 10U * 1024U * 1024U;
    if (len == 0 || len > MAX_MESSAGE_SIZE) {
        throw SocketException("invalid message size");
    }

    std::string msg(len, '\0');
    recv_all(socket, msg.data(), msg.size());
    validate_json_if_available(msg);
    return msg;
}

} // namespace velix::communication
