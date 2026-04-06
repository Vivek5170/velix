#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>
#include <random>
#include <string_view>

namespace velix::utils {

// Trim leading whitespace
inline std::string ltrim(std::string_view s) {
    std::size_t start = 0;
    while (start < s.size() && std::isspace(static_cast<unsigned char>(s[start]))) {
        ++start;
    }
    return std::string(s.substr(start));
}

// Trim trailing whitespace
inline std::string rtrim(std::string_view s) {
    std::size_t end = s.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
        --end;
    }
    return std::string(s.substr(0, end));
}

// Trim both leading and trailing whitespace
inline std::string trim(std::string_view s) {
    return rtrim(ltrim(s));
}

// Split string by delimiter
inline std::vector<std::string> split(std::string_view s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream{std::string(s)};
    while (std::getline(token_stream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline std::string join(const std::vector<std::string>& items, std::string_view delimiter) {
    if (items.empty()) {
        return "";
    }

    std::ostringstream oss;
    for (size_t i = 0; i < items.size(); ++i) {
        if (i > 0) {
            oss << delimiter;
        }
        oss << items[i];
    }
    return oss.str();
}

// Replace all occurrences of 'from' with 'to'
inline std::string replace_all(std::string s, std::string_view from, std::string_view to) {
    if (from.empty()) {
        return s;
    }

    size_t start_pos = 0;
    while ((start_pos = s.find(from, start_pos)) != std::string::npos) {
        s.replace(start_pos, from.length(), to.data(), to.length());
        start_pos += to.length();
    }
    return s;
}

// Convert string to lowercase
inline std::string to_lower(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Convert string to uppercase
inline std::string to_upper(std::string_view s) {
    std::string result(s);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

// Check if string starts with prefix
inline bool starts_with(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

// Check if string ends with suffix
inline bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool contains(std::string_view s, std::string_view needle) {
    return s.find(needle) != std::string::npos;
}

/**
 * Generates a simple 32-character random hex UUID string.
 * Strictly 0-dependency (only uses standard random library).
 */
inline std::string generate_uuid() {
    static std::random_device rd;
    static std::mt19937 gen(rd());
    static constexpr std::string_view hex_chars = "0123456789abcdef";
    static std::uniform_int_distribution<std::size_t> dis(0, hex_chars.size() - 1);
    
    std::string res;
    res.reserve(32);
    for (int i = 0; i < 32; ++i) {
        res += hex_chars[dis(gen)];
    }
    return res;
}

} // namespace velix::utils
