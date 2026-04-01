#pragma once

#include "../utils/config_utils.hpp"

#include <string>

namespace velix::communication {

inline std::string resolve_service_host(const std::string &service_name,
                                        const std::string &fallback = "127.0.0.1") {
  return velix::utils::get_service_host(service_name, fallback);
}

inline std::string resolve_bind_host(const std::string &service_name,
                                     const std::string &fallback = "127.0.0.1") {
  return velix::utils::get_bind_host(service_name, fallback);
}

} // namespace velix::communication
