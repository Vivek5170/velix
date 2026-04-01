#pragma once

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>
#include "../vendor/nlohmann/json.hpp"

namespace velix::utils {

namespace detail {
inline std::unordered_map<std::string, int> &get_ports_cache() {
    static std::unordered_map<std::string, int> cache;
    return cache;
}

inline nlohmann::json &get_config_cache() {
    static nlohmann::json cache;
    return cache;
}

inline std::mutex &get_config_mutex() {
    static std::mutex mtx;
    return mtx;
}

inline bool &ports_loaded() {
    static bool loaded = false;
    return loaded;
}

inline bool &config_loaded() {
    static bool loaded = false;
    return loaded;
}

inline void load_ports_impl() {
    auto &ports = get_ports_cache();
    std::ifstream pfile("config/ports.json");
    if (!pfile.is_open()) {
        pfile.open("../config/ports.json");
    }
    if (!pfile.is_open()) {
        pfile.open("build/config/ports.json");
    }
    if (pfile.is_open()) {
        try {
            nlohmann::json j;
            pfile >> j;
            for (auto it = j.begin(); it != j.end(); ++it) {
                if (it.value().is_number_integer()) ports[it.key()] = it.value().get<int>();
            }
        } catch (...) {}
    }
    ports_loaded() = true;
}

inline void load_configs_impl() {
    auto &config = get_config_cache();
    std::ifstream cfile("config/config.json");
    if (!cfile.is_open()) {
        cfile.open("../config/config.json");
    }
    if (!cfile.is_open()) {
        cfile.open("build/config/config.json");
    }
    if (cfile.is_open()) {
        try {
            cfile >> config;
        } catch (...) {
            config = nlohmann::json::object();
        }
    }
    config_loaded() = true;
}
} // namespace detail

/**
 * @brief Thread-safe port resolution with internal caching.
 */
inline int get_port(const std::string &name, int fallback) {
    std::lock_guard<std::mutex> lock(detail::get_config_mutex());
    if (!detail::ports_loaded()) {
        detail::load_ports_impl();
    }
    auto &cache = detail::get_ports_cache();
    auto it = cache.find(name);
    return (it != cache.end()) ? it->second : fallback;
}

/**
 * @brief Resolve service host from config/config.json key SERVICE_HOSTS.<name>.
 * Falls back to provided value when not configured.
 */
inline std::string get_service_host(const std::string &name, const std::string &fallback) {
    std::lock_guard<std::mutex> lock(detail::get_config_mutex());
    if (!detail::config_loaded()) {
        detail::load_configs_impl();
    }
    auto &config = detail::get_config_cache();
    try {
        if (config.contains("SERVICE_HOSTS") && config["SERVICE_HOSTS"].is_object()) {
            const auto &hosts = config["SERVICE_HOSTS"];
            if (hosts.contains(name) && hosts[name].is_string()) {
                const std::string value = hosts[name].get<std::string>();
                if (!value.empty()) {
                    return value;
                }
            }
        }
    } catch (...) {}
    return fallback;
}

/**
 * @brief Resolve service bind host from config/config.json key BIND_HOSTS.<name>.
 * Falls back to provided value when not configured.
 */
inline std::string get_bind_host(const std::string &name, const std::string &fallback) {
    std::lock_guard<std::mutex> lock(detail::get_config_mutex());
    if (!detail::config_loaded()) {
        detail::load_configs_impl();
    }
    auto &config = detail::get_config_cache();
    try {
        if (config.contains("BIND_HOSTS") && config["BIND_HOSTS"].is_object()) {
            const auto &hosts = config["BIND_HOSTS"];
            if (hosts.contains(name) && hosts[name].is_string()) {
                const std::string value = hosts[name].get<std::string>();
                if (!value.empty()) {
                    return value;
                }
            }
        }
    } catch (...) {}
    return fallback;
}

/**
 * @brief Retrieves a typed value from config/config.json with a fallback.
 */
template <typename T>
inline T get_config(const std::string &key, T fallback) {
    std::lock_guard<std::mutex> lock(detail::get_config_mutex());
    if (!detail::config_loaded()) {
        detail::load_configs_impl();
    }
    auto &config = detail::get_config_cache();
    
    if (config.contains(key)) {
        try {
            return config.at(key).get<T>();
        } catch (...) {
            return fallback;
        }
    }
    return fallback;
}

/**
 * @brief Forces a re-read of configuration files from disk.
 */
inline void reload_configs() {
    std::lock_guard<std::mutex> lock(detail::get_config_mutex());
    detail::get_ports_cache().clear();
    detail::get_config_cache().clear();
    detail::ports_loaded() = false;
    detail::config_loaded() = false;
    detail::load_configs_impl();
    detail::load_ports_impl();
}

} // namespace velix::utils
