#pragma once

#include <string>
#include <cstring>
#include <stdexcept>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using SocketHandle = SOCKET;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <signal.h>
using SocketHandle = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

namespace velix::communication
{

    class SocketException : public std::runtime_error
    {
    public:
        explicit SocketException(const std::string &msg) : std::runtime_error(msg) {}
    };

    /**
     * Initialize Winsock once at program startup (Windows only).
     * Call this exactly once before creating any sockets.
     * On POSIX systems, this is a no-op and disables SIGPIPE.
     */
    inline void init_winsock_global()
    {
#ifdef _WIN32
        static bool initialized = false;
        if (!initialized)
        {
            WSADATA wsa_data;
            int result = WSAStartup(MAKEWORD(2, 2), &wsa_data);
            if (result != 0)
            {
                throw SocketException("WSAStartup failed: " + std::to_string(result));
            }
            initialized = true;
        }
#else
        // POSIX: disable SIGPIPE globally to prevent crashes on closed sockets
        static bool sigpipe_disabled = false;
        if (!sigpipe_disabled)
        {
            signal(SIGPIPE, SIG_IGN);
            sigpipe_disabled = true;
        }
#endif
    }

    /**
     * Helper to get human-readable error message from errno/WSAGetLastError.
     */
    inline std::string get_socket_error()
    {
#ifdef _WIN32
        int err = WSAGetLastError();
        return "WSA error " + std::to_string(err);
#else
        int err = errno;
        return "errno " + std::to_string(err) + ": " + strerror(err);
#endif
    }

    class SocketWrapper
    {
    private:
        SocketHandle socket_handle;

    public:
        SocketWrapper() : socket_handle(INVALID_SOCKET_HANDLE)
        {
            init_winsock_global();
        }

        ~SocketWrapper()
        {
            close();
        }

        // Delete copy operations
        SocketWrapper(const SocketWrapper &) = delete;
        SocketWrapper &operator=(const SocketWrapper &) = delete;

        // Move operations
        SocketWrapper(SocketWrapper &&other) noexcept : socket_handle(other.socket_handle)
        {
            other.socket_handle = INVALID_SOCKET_HANDLE;
        }

        SocketWrapper &operator=(SocketWrapper &&other) noexcept
        {
            if (this != &other)
            {
                close();
                socket_handle = other.socket_handle;
                other.socket_handle = INVALID_SOCKET_HANDLE;
            }
            return *this;
        }

        void create_tcp_socket()
        {
            if (socket_handle != INVALID_SOCKET_HANDLE)
            {
                close();
            }
            socket_handle = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Failed to create TCP socket: " + get_socket_error());
            }
        }

        void bind(const std::string &address, uint16_t port)
        {
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Socket not created");
            }

            // Fix Issue #5: Enable SO_REUSEADDR to allow immediate rebind after restart
            int reuse = 1;
#ifdef _WIN32
            if (setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                           (char *)&reuse, sizeof(reuse)) == SOCKET_ERROR)
            {
#else
            if (setsockopt(socket_handle, SOL_SOCKET, SO_REUSEADDR,
                           &reuse, sizeof(reuse)) == -1)
            {
#endif
                throw SocketException("setsockopt SO_REUSEADDR failed: " + get_socket_error());
            }

            sockaddr_in server_addr;
            std::memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(port);

            int result = inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr);
            if (result <= 0)
            {
                throw SocketException("Invalid address: " + address);
            }

#ifdef _WIN32
            if (::bind(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
            {
#else
            if (::bind(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) == -1)
            {
#endif
                throw SocketException("Bind failed on " + address + ":" + std::to_string(port) +
                                      " (" + get_socket_error() + ")");
            }
        }

        void listen(int backlog = 5)
        {
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Socket not created");
            }
#ifdef _WIN32
            if (::listen(socket_handle, backlog) == SOCKET_ERROR)
            {
#else
            if (::listen(socket_handle, backlog) == -1)
            {
#endif
                throw SocketException("Listen failed: " + get_socket_error());
            }
        }

        SocketWrapper accept()
        {
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Socket not created");
            }

            sockaddr_in client_addr;
            socklen_t client_addr_len = sizeof(client_addr);
            std::memset(&client_addr, 0, sizeof(client_addr));

#ifdef _WIN32
            SocketHandle client_socket = ::accept(socket_handle, (sockaddr *)&client_addr, &client_addr_len);
            if (client_socket == INVALID_SOCKET)
            {
#else
            SocketHandle client_socket = ::accept(socket_handle, (sockaddr *)&client_addr, &client_addr_len);
            if (client_socket == -1)
            {
#endif
                throw SocketException("Accept failed: " + get_socket_error());
            }

            SocketWrapper client;
            client.socket_handle = client_socket;
            return client;
        }

        void connect(const std::string &address, uint16_t port)
        {
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Socket not created");
            }

            sockaddr_in server_addr;
            std::memset(&server_addr, 0, sizeof(server_addr));
            server_addr.sin_family = AF_INET;
            server_addr.sin_port = htons(port);

            int result = inet_pton(AF_INET, address.c_str(), &server_addr.sin_addr);
            if (result <= 0)
            {
                throw SocketException("Invalid address: " + address);
            }

#ifdef _WIN32
            if (::connect(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) == SOCKET_ERROR)
            {
#else
            if (::connect(socket_handle, (sockaddr *)&server_addr, sizeof(server_addr)) == -1)
            {
#endif
                throw SocketException("Connection failed to " + address + ":" + std::to_string(port) +
                                      " (" + get_socket_error() + ")");
            }
        }

        /**
         * Set socket timeout (in milliseconds).
         * Works on both Windows and POSIX systems.
         * Issue #6: Production systems need timeouts.
         */
        void set_timeout_ms(int timeout_ms)
        {
            if (socket_handle == INVALID_SOCKET_HANDLE)
            {
                throw SocketException("Socket not created");
            }

#ifdef _WIN32
            // Windows uses milliseconds directly
            if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO,
                           (char *)&timeout_ms, sizeof(timeout_ms)) == SOCKET_ERROR)
            {
                throw SocketException("setsockopt SO_RCVTIMEO failed: " + get_socket_error());
            }
            if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO,
                           (char *)&timeout_ms, sizeof(timeout_ms)) == SOCKET_ERROR)
            {
                throw SocketException("setsockopt SO_SNDTIMEO failed: " + get_socket_error());
            }
#else
            // POSIX uses struct timeval
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            if (setsockopt(socket_handle, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) == -1)
            {
                throw SocketException("setsockopt SO_RCVTIMEO failed: " + get_socket_error());
            }
            if (setsockopt(socket_handle, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)) == -1)
            {
                throw SocketException("setsockopt SO_SNDTIMEO failed: " + get_socket_error());
            }
#endif
        }

        int send(const char *data, int len)
        {
#ifdef _WIN32
            int result = ::send(socket_handle, data, len, 0);
            if (result == SOCKET_ERROR)
            {
                throw SocketException("Send failed: " + get_socket_error());
            }
#else
            // Fix Issue #4: Use MSG_NOSIGNAL to prevent SIGPIPE on closed sockets
            int result = ::send(socket_handle, data, len, MSG_NOSIGNAL);
            if (result == -1)
            {
                throw SocketException("Send failed: " + get_socket_error());
            }
#endif
            return result;
        }

        int recv(char *buffer, int len)
        {
#ifdef _WIN32
            int result = ::recv(socket_handle, buffer, len, 0);
            if (result == SOCKET_ERROR)
            {
                throw SocketException("Recv failed: " + get_socket_error());
            }
#else
            // Fix Issue #3: Handle EINTR (interrupted system call) - retry on signal
            int result;
            do
            {
                result = ::recv(socket_handle, buffer, len, 0);
            } while (result == -1 && errno == EINTR);

            if (result == -1)
            {
                throw SocketException("Recv failed: " + get_socket_error());
            }
#endif
            return result;
        }

        void close()
        {
            if (socket_handle != INVALID_SOCKET_HANDLE)
            {
#ifdef _WIN32
                closesocket(socket_handle);
#else
                ::close(socket_handle);
#endif
                socket_handle = INVALID_SOCKET_HANDLE;
            }
        }

        bool is_open() const
        {
            return socket_handle != INVALID_SOCKET_HANDLE;
        }

        SocketHandle get_handle() const
        {
            return socket_handle;
        }
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
