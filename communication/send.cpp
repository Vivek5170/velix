#include "socket_wrapper.hpp"

#include <cstdint>
#include <cstring>
#include <chrono>
#include "../utils/config_utils.hpp"

#include "json_validate.hpp"

namespace velix::communication {

static void send_all(SocketWrapper& socket, const char* data, std::size_t len) {
    std::size_t total = 0;
    // Total time we are willing to block waiting for socket to become
    // writable (milliseconds). Use a config value with a conservative default.
    const int kDefaultSendTimeoutMs = 2000;
    const int send_timeout_ms = static_cast<int>(
        velix::utils::get_config("VELIX_COMM_SEND_TIMEOUT_MS", kDefaultSendTimeoutMs));

    // We'll track the absolute deadline and use select to wait for writeable.
    auto start = std::chrono::steady_clock::now();
    auto deadline = start + std::chrono::milliseconds(send_timeout_ms);

    while (total < len) {
        auto sent = socket.send(data + total, static_cast<int>(len - total));
        if (sent > 0) {
            total += static_cast<std::size_t>(sent);
            continue;
        }

#ifndef _WIN32
        // If send returned -1 (SocketWrapper returns -1 for EAGAIN/EWOULDBLOCK)
        // or other errors, inspect errno. SocketWrapper already retries EINTR.
        if (sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            // Compute remaining time
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                throw SocketException("send timeout");
            }
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();

            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(socket.get_handle(), &wfds);

            struct timeval tv;
            tv.tv_sec = static_cast<long>(remaining / 1000);
            tv.tv_usec = static_cast<long>((remaining % 1000) * 1000);

            int r = select(static_cast<int>(socket.get_handle()) + 1, nullptr, &wfds, nullptr, &tv);
            if (r > 0) {
                // socket is writable, retry send
                continue;
            } else if (r == 0) {
                // timeout expired on this select call — loop will check overall deadline
                continue;
            } else {
                // select error
                throw SocketException(std::string("select failed: ") + get_socket_error());
            }
        }
#endif

        // Treat send == 0 (peer closed) or other errors as fatal.
        if (sent == 0) {
            throw SocketException("send returned 0 (connection closed)");
        }
        throw SocketException("send failed");
    }
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
