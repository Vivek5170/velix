#pragma once

#include <chrono>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <cstdlib>
#include <cctype>
#include "../communication/json_include.hpp"

namespace velix::utils {

struct TransparentStringHash {
    using is_transparent = void;

    std::size_t operator()(std::string_view value) const noexcept {
        return std::hash<std::string_view>{}(value);
    }

    std::size_t operator()(const std::string& value) const noexcept {
        return operator()(std::string_view(value));
    }

    std::size_t operator()(const char* value) const noexcept {
        return operator()(std::string_view(value));
    }
};

struct TransparentStringEqual {
    using is_transparent = void;

    bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
        return lhs == rhs;
    }

    bool operator()(const std::string& lhs, const std::string& rhs) const noexcept {
        return lhs == rhs;
    }

    bool operator()(const char* lhs, const char* rhs) const noexcept {
        return std::string_view(lhs) == std::string_view(rhs);
    }
};

using DotEnvMap = std::unordered_map<std::string, std::string, TransparentStringHash, TransparentStringEqual>;

namespace detail {

using SteadyClock = std::chrono::steady_clock;
using TimePoint   = SteadyClock::time_point;

// TTL for config cache, configurable via VELIX_CONFIG_TTL_SEC env var.
inline int config_ttl_sec() {
    static const int ttl = []() {
        if (const char *v = std::getenv("VELIX_CONFIG_TTL_SEC"); v && *v) {
            try { return std::stoi(v); } catch (...) {}
        }
        return 5; // default 5 seconds
    }();
    return ttl;
}

inline std::unordered_map<std::string, int, TransparentStringHash, TransparentStringEqual> &get_ports_cache() {
    static std::unordered_map<std::string, int, TransparentStringHash, TransparentStringEqual> cache;
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

inline TimePoint &ports_loaded_at() {
    static TimePoint t{};
    return t;
}

inline TimePoint &config_loaded_at() {
    static TimePoint t{};
    return t;
}

inline bool is_stale(const TimePoint &loaded_at) {
    if (loaded_at == TimePoint{}) return true; // never loaded
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        SteadyClock::now() - loaded_at).count();
    return elapsed >= config_ttl_sec();
}

inline void load_ports_impl() {
    auto &ports = get_ports_cache();
    ports.clear();
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
        } catch (const nlohmann::json::exception&) {
            // Keep empty cache on malformed ports file.
        }
    }
    ports_loaded_at() = SteadyClock::now();
}

inline void load_configs_impl() {
    auto &config = get_config_cache();
    config = nlohmann::json::object();
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
        } catch (const nlohmann::json::exception&) {
            config = nlohmann::json::object();
        }
    }
    config_loaded_at() = SteadyClock::now();
}
} // namespace detail

/**
 * @brief Thread-safe port resolution with internal caching.
 */
inline int get_port(const std::string &name, int fallback) {
    std::scoped_lock lock(detail::get_config_mutex());
    if (detail::is_stale(detail::ports_loaded_at())) {
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
    std::scoped_lock lock(detail::get_config_mutex());
    if (detail::is_stale(detail::config_loaded_at())) {
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
    } catch (const nlohmann::json::exception&) {
        // Fall back on malformed config structure.
    }
    return fallback;
}

/**
 * @brief Resolve service bind host from config/config.json key BIND_HOSTS.<name>.
 * Falls back to provided value when not configured.
 */
inline DotEnvMap load_dotenv(const std::string &path) {
    DotEnvMap map;
    std::ifstream in(path);
    if (!in.is_open()) {
        return map;
    }

    std::string line;
    while (std::getline(in, line)) {
        auto trimmed = line;
        while (!trimmed.empty() && isspace(static_cast<unsigned char>(trimmed.front()))) trimmed.erase(trimmed.begin());
        while (!trimmed.empty() && isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
        if (trimmed.empty() || trimmed.front() == '#') continue;
        auto eq = trimmed.find('=');
        if (eq == std::string::npos) continue;
        std::string key = trimmed.substr(0, eq);
        std::string val = trimmed.substr(eq + 1);
        while (!key.empty() && isspace(static_cast<unsigned char>(key.back()))) key.pop_back();
        while (!val.empty() && isspace(static_cast<unsigned char>(val.front()))) val.erase(val.begin());
        while (!val.empty() && isspace(static_cast<unsigned char>(val.back()))) val.pop_back();
        if (val.size() >= 2 && val.front() == '"' && val.back() == '"') {
            val = val.substr(1, val.size() - 2);
        }
        map[key] = val;
    }
    return map;
}

inline std::string get_env_value(const std::string &name, const DotEnvMap &dot_env) {
    if (auto it = dot_env.find(name); it != dot_env.end() && !it->second.empty()) {
        return it->second;
    }
#ifdef _WIN32
    char *value = nullptr;
    std::size_t len = 0;
    const errno_t err = _dupenv_s(&value, &len, name.c_str());
    if (err != 0 || value == nullptr) {
        return "";
    }
    std::string out(value);
    std::free(value);
    return out;
#else
    if (const char *v = std::getenv(name.c_str()); v != nullptr) {
        return std::string(v);
    }
    return "";
#endif
}

inline std::string get_bind_host(const std::string &name, const std::string &fallback) {
    std::scoped_lock lock(detail::get_config_mutex());
    if (detail::is_stale(detail::config_loaded_at())) {
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
    } catch (const nlohmann::json::exception&) {
        // Fall back on malformed config structure.
    }
    return fallback;
}

/**
 * @brief Retrieves a typed value from config/config.json with a fallback.
 */
template <typename T>
inline T get_config(const std::string &key, T fallback) {
    std::scoped_lock lock(detail::get_config_mutex());
    if (detail::is_stale(detail::config_loaded_at())) {
        detail::load_configs_impl();
    }
    auto &config = detail::get_config_cache();
    
    if (config.contains(key)) {
        try {
            return config.at(key).get<T>();
        } catch (const nlohmann::json::exception&) {
            return fallback;
        }
    }
    return fallback;
}

/**
 * @brief Forces a re-read of configuration files from disk.
 */
inline void reload_configs() {
    std::scoped_lock lock(detail::get_config_mutex());
    detail::get_ports_cache().clear();
    detail::get_config_cache().clear();
    detail::ports_loaded_at() = detail::TimePoint{};
    detail::config_loaded_at() = detail::TimePoint{};
    detail::load_configs_impl();
    detail::load_ports_impl();
}

} // namespace velix::utils
