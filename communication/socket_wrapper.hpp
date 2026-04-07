#pragma once

#include <cstring>
#include <stdexcept>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#ifdef ERROR
#undef ERROR
#endif
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/socket.h>
#include <unistd.h>
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

namespace velix::communication {

class SocketException : public std::runtime_error {
public:
  using std::runtime_error::runtime_error;
};

class SocketTimeoutException : public SocketException {
public:
  using SocketException::SocketException;
};

/**
 * Initialize Winsock once at program startup (Windows only).
 * Call this exactly once before creating any sockets.
 * On POSIX systems, this is a no-op and disables SIGPIPE.
 */
inline void init_winsock_global() {
#ifdef _WIN32
  static bool initialized = false;
  if (!initialized) {
    WSADATA wsa_data;
    if (const int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
        result != 0) {
      throw SocketException("WSAStartup failed: " + std::to_string(result));
    }
    initialized = true;
  }
#else
  // POSIX: disable SIGPIPE globally to prevent crashes on closed sockets
  static bool sigpipe_disabled = false;
  if (!sigpipe_disabled) {
    signal(SIGPIPE, SIG_IGN);
    sigpipe_disabled = true;
  }
#endif
}

/**
 * Helper to get human-readable error message from errno/WSAGetLastError.
 */
inline std::string get_socket_error() {
#ifdef _WIN32
  int err = WSAGetLastError();
  return "WSA error " + std::to_string(err);
#else
  int err = errno;
  return "errno " + std::to_string(err) + ": " + strerror(err);
#endif
}

class SocketWrapper {
private:
  SocketHandle socket_handle{INVALID_SOCKET_HANDLE};

#ifndef _WIN32
  static void set_close_on_exec(SocketHandle handle) {
    if (handle == INVALID_SOCKET_HANDLE) {
      return;
    }

    const int flags = fcntl(handle, F_GETFD);
    if (flags == -1) {
      throw SocketException("fcntl(F_GETFD) failed: " + get_socket_error());
    }

    if (fcntl(handle, F_SETFD, flags | FD_CLOEXEC) == -1) {
      throw SocketException("fcntl(F_SETFD, FD_CLOEXEC) failed: " +
                            get_socket_error());
    }
  }
#endif

public:
  SocketWrapper() {
    init_winsock_global();
  }

  ~SocketWrapper() { close(); }

  // Delete copy operations
  SocketWrapper(const SocketWrapper &) = delete;
  SocketWrapper &operator=(const SocketWrapper &) = delete;

  // Move operations
  SocketWrapper(SocketWrapper &&other) noexcept
      : socket_handle(other.socket_handle) {
    other.socket_handle = INVALID_SOCKET_HANDLE;
  }

  SocketWrapper &operator=(SocketWrapper &&other) noexcept {
    if (this != &other) {
      close();
      socket_handle = other.socket_handle;
      other.socket_handle = INVALID_SOCKET_HANDLE;
    }
    return *this;
  }

  void create_tcp_socket() {
    if (socket_handle != INVALID_SOCKET_HANDLE) {
      close();
    }
    socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Failed to create TCP socket: " +
                            get_socket_error());
    }

#ifndef _WIN32
    set_close_on_exec(socket_handle);
#endif
  }

  void bind(const std::string &address, uint16_t port) const {
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Socket not created");
    }

    // Fix Issue #5: Enable SO_REUSEADDR to allow immediate rebind after restart
#ifdef _WIN32
    if (const int reuse = 1;
        setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                   reinterpret_cast<const char *>(&reuse),
                   sizeof(reuse)) == SOCKET_ERROR) {
#else
    if (const int reuse = 1;
        setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR, &reuse,
                   sizeof(reuse)) == -1) {
#endif
      throw SocketException("setsockopt SO_REUSEADDR failed: " +
                            get_socket_error());
    }

    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (const int result =
            inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr);
        result <= 0) {
      throw SocketException("Invalid address: " + address);
    }

#ifdef _WIN32
    if (::bind(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) ==
        SOCKET_ERROR) {
#else
    if (::bind(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) ==
        -1) {
#endif
      throw SocketException("Bind failed on " + address + ":" +
                            std::to_string(port) + " (" + get_socket_error() +
                            ")");
    }
  }

  void listen(int backlog = 5) const {
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Socket not created");
    }
#ifdef _WIN32
    if (::listen(socket_handle, backlog) == SOCKET_ERROR) {
#else
    if (::listen(socket_handle, backlog) == -1) {
#endif
      throw SocketException("Listen failed: " + get_socket_error());
    }
  }

  SocketWrapper accept() const {
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Socket not created");
    }

    sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    std::memset(&client_addr, 0, sizeof(client_addr));

#ifdef _WIN32
    SocketHandle client_socket =
        ::accept(socket_handle, (sockaddr *)&client_addr, &client_addr_len);
    if (client_socket == INVALID_SOCKET) {
#else
    SocketHandle client_socket =
        ::accept(socket_handle, (sockaddr *)&client_addr, &client_addr_len);
    if (client_socket == -1) {
#endif
      throw SocketException("Accept failed: " + get_socket_error());
    }

    SocketWrapper client;
    client.socket_handle = client_socket;
#ifndef _WIN32
    set_close_on_exec(client.socket_handle);
#endif
    return client;
  }

  void connect(const std::string &address, uint16_t port) const {
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Socket not created");
    }

    sockaddr_in server_addr;
    std::memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (const int result =
            inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr);
        result <= 0) {
      throw SocketException("Invalid address: " + address);
    }

#ifdef _WIN32
    if (::connect(socket_handle, (sockaddr *)&server_addr,
                  sizeof(server_addr)) == SOCKET_ERROR) {
#else
    if (::connect(socket_handle, (sockaddr *)&server_addr,
                  sizeof(server_addr)) == -1) {
#endif
      throw SocketException("Connection failed to " + address + ":" +
                            std::to_string(port) + " (" + get_socket_error() +
                            ")");
    }
  }

  /**
   * Set socket timeout (in milliseconds).
   * Works on both Windows and POSIX systems.
   * Issue #6: Production systems need timeouts.
   */
  void set_timeout_ms(int timeout_ms) const {
    if (socket_handle == INVALID_SOCKET_HANDLE) {
      throw SocketException("Socket not created");
    }

#ifdef _WIN32
    // Windows uses milliseconds directly
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
             reinterpret_cast<const char *>(&timeout_ms),
             sizeof(timeout_ms)) == SOCKET_ERROR) {
      throw SocketException("setsockopt SO_RCVTIMEO failed: " +
                            get_socket_error());
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
             reinterpret_cast<const char *>(&timeout_ms),
             sizeof(timeout_ms)) == SOCKET_ERROR) {
      throw SocketException("setsockopt SO_SNDTIMEO failed: " +
                            get_socket_error());
    }
#else
    // POSIX uses struct timeval
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) ==
        -1) {
      throw SocketException("setsockopt SO_RCVTIMEO failed: " +
                            get_socket_error());
    }
    if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) ==
        -1) {
      throw SocketException("setsockopt SO_SNDTIMEO failed: " +
                            get_socket_error());
    }
#endif
  }

  int send(const char *data, int len) const {
#ifdef _WIN32
    if (const int result = ::send(socket_handle, data, len, 0);
        result == SOCKET_ERROR) {
      throw SocketException("Send failed: " + get_socket_error());
    } else {
      return result;
    }
#else
    // Use MSG_NOSIGNAL when available (Linux). On platforms where it
    // is unavailable (e.g., macOS/BSD), SIGPIPE is already disabled by
    // init_winsock_global() via signal(SIGPIPE, SIG_IGN).
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif
    if (const int result = ::send(socket_handle, data, len, flags);
        result == -1) {
      throw SocketException("Send failed: " + get_socket_error());
    } else {
      return result;
    }
#endif
  }

  int recv(char *buffer, int len) const {
#ifdef _WIN32
    if (const int result = ::recv(socket_handle, buffer, len, 0);
        result == SOCKET_ERROR) {
      if (const int err = WSAGetLastError();
          err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
        throw SocketTimeoutException("Recv timeout: " + get_socket_error());
      }
      throw SocketException("Recv failed: " + get_socket_error());
    } else {
      return result;
    }
#else
    // Fix Issue #3: Handle EINTR (interrupted system call) - retry on signal
    int result;
    do {
      result = ::recv(socket_handle, buffer, len, 0);
    } while (result == -1 && errno == EINTR);

    if (result == -1) {
      if (errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT) {
        throw SocketTimeoutException("Recv timeout: " + get_socket_error());
      }
      throw SocketException("Recv failed: " + get_socket_error());
    }
    return result;
#endif
  }

  void close() {
    if (socket_handle != INVALID_SOCKET_HANDLE) {
#ifdef _WIN32
      closesocket(socket_handle);
#else
      ::close(socket_handle);
#endif
      socket_handle = INVALID_SOCKET_HANDLE;
    }
  }

  bool is_open() const { return socket_handle != INVALID_SOCKET_HANDLE; }

  bool has_data(int timeout_ms = 0) const {
    if (socket_handle == INVALID_SOCKET_HANDLE)
      return false;
    fd_set read_fds;
    FD_ZERO(&read_fds);
    FD_SET(socket_handle, &read_fds);

    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;

    int result = select(static_cast<int>(socket_handle) + 1, &read_fds, nullptr,
                        nullptr, &tv);
    return result > 0;
  }

  SocketHandle get_handle() const { return socket_handle; }
};

/**
 * Sends a complete JSON message using length-prefixed framing.
 * Frame format: [4-byte big-endian length][raw JSON bytes].
 */
void send_json(SocketWrapper &socket, const std::string &json);

/**
 * Receives a complete JSON message using length-prefixed framing.
 * Frame format: [4-byte big-endian length][raw JSON bytes].
 */
std::string recv_json(SocketWrapper &socket);

} // namespace velix::communication
