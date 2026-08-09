#pragma once

#include "json_include.hpp"

#include <asio.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace velix::communication {

inline std::uint32_t decode_frame_length(
    const std::array<unsigned char, 4> &header) {
  return (static_cast<std::uint32_t>(header[0]) << 24) |
         (static_cast<std::uint32_t>(header[1]) << 16) |
         (static_cast<std::uint32_t>(header[2]) << 8) |
         static_cast<std::uint32_t>(header[3]);
}

inline void encode_frame_length(std::uint32_t length,
                                std::array<unsigned char, 4> &header) {
  header[0] = static_cast<unsigned char>((length >> 24) & 0xff);
  header[1] = static_cast<unsigned char>((length >> 16) & 0xff);
  header[2] = static_cast<unsigned char>((length >> 8) & 0xff);
  header[3] = static_cast<unsigned char>(length & 0xff);
}

inline std::string make_frame(std::string_view payload) {
  std::array<unsigned char, 4> header{};
  encode_frame_length(static_cast<std::uint32_t>(payload.size()), header);

  std::string frame;
  frame.resize(header.size() + payload.size());
  std::memcpy(frame.data(), header.data(), header.size());
  if (!payload.empty()) {
    std::memcpy(frame.data() + header.size(), payload.data(), payload.size());
  }
  return frame;
}

template <typename AsyncReadStream>
void async_read_frame(
    AsyncReadStream &stream, std::uint32_t max_message_bytes,
    std::function<void(const asio::error_code &, std::string)> handler) {
  auto header = std::make_shared<std::array<unsigned char, 4>>();
  asio::async_read(
      stream, asio::buffer(*header),
      [&stream, header, max_message_bytes,
       handler = std::move(handler)](const asio::error_code &ec,
                                     std::size_t) mutable {
        if (ec) {
          handler(ec, {});
          return;
        }

        const std::uint32_t length = decode_frame_length(*header);
        if (length == 0 || length > max_message_bytes) {
          handler(asio::error::make_error_code(asio::error::message_size), {});
          return;
        }

        auto payload = std::make_shared<std::string>();
        payload->resize(length);
        asio::async_read(
            stream, asio::buffer(*payload),
            [payload, handler = std::move(handler)](
                const asio::error_code &payload_ec, std::size_t) mutable {
              handler(payload_ec, payload_ec ? std::string{} : std::move(*payload));
            });
      });
}

} // namespace velix::communication
