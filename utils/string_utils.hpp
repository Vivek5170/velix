#pragma once

#include <string>
#include <vector>
#include <algorithm>
#include <cctype>
#include <sstream>

namespace velix::utils {

// Trim leading whitespace
inline std::string ltrim(const std::string& s) {
    auto start = s.begin();
    while (start != s.end() && std::isspace(static_cast<unsigned char>(*start))) {
        start++;
    }
    return std::string(start, s.end());
}

// Trim trailing whitespace
inline std::string rtrim(const std::string& s) {
    auto end = s.rbegin();
    while (end != s.rend() && std::isspace(static_cast<unsigned char>(*end))) {
        end++;
    }
    return std::string(s.begin(), end.base());
}

// Trim both leading and trailing whitespace
inline std::string trim(const std::string& s) {
    return rtrim(ltrim(s));
}

// Split string by delimiter
inline std::vector<std::string> split(const std::string& s, char delimiter) {
    std::vector<std::string> tokens;
    std::string token;
    std::istringstream token_stream(s);
    while (std::getline(token_stream, token, delimiter)) {
        tokens.push_back(token);
    }
    return tokens;
}

inline std::string join(const std::vector<std::string>& items, const std::string& delimiter) {
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
inline std::string replace_all(std::string s, const std::string& from, const std::string& to) {
    if (from.empty()) {
        return s;
    }

    size_t start_pos = 0;
    while ((start_pos = s.find(from, start_pos)) != std::string::npos) {
        s.replace(start_pos, from.length(), to);
        start_pos += to.length();
    }
    return s;
}

// Convert string to lowercase
inline std::string to_lower(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return result;
}

// Convert string to uppercase
inline std::string to_upper(const std::string& s) {
    std::string result = s;
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return result;
}

// Check if string starts with prefix
inline bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() &&
           s.compare(0, prefix.size(), prefix) == 0;
}

// Check if string ends with suffix
inline bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() &&
           s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

inline bool contains(const std::string& s, const std::string& needle) {
    return s.find(needle) != std::string::npos;
}

} // namespace velix::utils
