#include "socket_wrapper.hpp"

#include <cstdint>
#include "../utils/config_utils.hpp"

#include "json_include.hpp"
inline constexpr bool kHasNlohmannJson = true;

#include "json_validate.hpp"

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

// Read raw payload (no JSON validation) and return it. This function enforces
// the configurable maximum message size and reads directly into a pre-sized
// std::string to avoid extra copies.
std::string recv_raw(SocketWrapper& socket) {
    if (!socket.is_open()) {
        throw SocketException("socket not open");
    }

    std::uint32_t net_len = 0;
    recv_all(socket, reinterpret_cast<char*>(&net_len), sizeof(net_len));

    std::uint32_t len = ntohl(net_len);

    // Configurable maximum message size (security boundary)
    const std::uint32_t kDefaultMax = 10U * 1024U * 1024U; // 10 MB
    const std::uint32_t max_size =
        static_cast<std::uint32_t>(velix::utils::get_config("VELIX_COMM_MAX_MESSAGE_SIZE", kDefaultMax));
    if (len == 0 || len > max_size) {
        throw SocketException("invalid message size");
    }

    // Allocate once and read directly into the string buffer to avoid extra
    // copies while keeping memory bounded by max_size.
    std::string msg;
    msg.resize(static_cast<std::size_t>(len));

    const std::size_t kChunkSize = 64 * 1024; // 64 KB chunks
    std::size_t offset = 0;
    while (offset < static_cast<std::size_t>(len)) {
        const std::size_t to_read = std::min<std::size_t>(kChunkSize, static_cast<std::size_t>(len) - offset);
        recv_all(socket, msg.data() + offset, to_read);
        offset += to_read;
    }

    return msg;
}

nlohmann::json recv_json_parsed(SocketWrapper& socket) {
    const std::string raw = recv_raw(socket);
    try {
        return nlohmann::json::parse(raw);
    } catch (const std::exception& e) {
        throw SocketException(std::string("Invalid JSON: ") + e.what());
    }
}

// Backwards-compatible helper: reads and validates JSON, returns raw string.
std::string recv_json(SocketWrapper& socket) {
    const std::string raw = recv_raw(socket);
    if (kHasNlohmannJson) {
        validate_json_if_available(raw);
    }
    return raw;
}

} // namespace velix::communication
