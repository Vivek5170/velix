#pragma once

#include <string>
#include <fstream>
#include <iostream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <filesystem>
#include <mutex>

namespace velix::utils {

struct LogContext {
    std::string module = "unknown";
    std::string tree_id = "";
    int pid = -1;
    std::string event = "";
};

class Logger {
private:
    inline static std::string log_dir = "logs";
    inline static std::ofstream log_file;
    inline static bool initialized = false;
    inline static std::mutex write_mutex;

    static std::string escape_json(const std::string& value) {
        std::string out;
        out.reserve(value.size() + 8);
        for (char c : value) {
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: out += c; break;
            }
        }
        return out;
    }

    static std::string get_timestamp() {
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time);
#else
        localtime_r(&time, &tm_buf);
#endif

        std::ostringstream oss;
        oss << std::put_time(&tm_buf, "%Y-%m-%d %H:%M:%S")
            << '.' << std::setfill('0') << std::setw(3) << ms.count();
        return oss.str();
    }

    static std::string build_json_line(const std::string& level,
                                       const std::string& message,
                                       const LogContext& context) {
        std::ostringstream oss;
        oss << "{"
            << "\"timestamp\":\"" << escape_json(get_timestamp()) << "\"," 
            << "\"level\":\"" << escape_json(level) << "\"," 
            << "\"module\":\"" << escape_json(context.module) << "\"," 
            << "\"tree_id\":\"" << escape_json(context.tree_id) << "\"," 
            << "\"pid\":" << context.pid << ","
            << "\"event\":\"" << escape_json(context.event) << "\"," 
            << "\"message\":\"" << escape_json(message) << "\""
            << "}";
        return oss.str();
    }

    static void ensure_initialized() {
        if (!initialized) {
            init("logs");
        }
    }

public:
    static void init(const std::string& directory = "logs") {
        std::lock_guard<std::mutex> lock(write_mutex);
        log_dir = directory;
        std::filesystem::create_directories(log_dir);
        
        std::string log_file_path = log_dir + "/velix.log";
        if (log_file.is_open()) {
            log_file.close();
        }
        log_file.open(log_file_path, std::ios::app);
        
        if (!log_file.is_open()) {
            std::cerr << "Failed to open log file: " << log_file_path << std::endl;
        }
        initialized = true;
    }

    static void log(const std::string& level,
                    const std::string& message,
                    const LogContext& context = LogContext{}) {
        ensure_initialized();
        const std::string line = build_json_line(level, message, context);
        std::lock_guard<std::mutex> lock(write_mutex);

        if (level == "ERROR") {
            std::cerr << line << std::endl;
        } else {
            std::cout << line << std::endl;
        }

        if (log_file.is_open()) {
            log_file << line << std::endl;
            log_file.flush();
        }
    }

    static void info(const std::string& message) {
        log("INFO", message);
    }

    static void info(const std::string& message, const LogContext& context) {
        log("INFO", message, context);
    }

    static void error(const std::string& message) {
        log("ERROR", message);
    }

    static void error(const std::string& message, const LogContext& context) {
        log("ERROR", message, context);
    }

    static void debug(const std::string& message) {
        log("DEBUG", message);
    }

    static void debug(const std::string& message, const LogContext& context) {
        log("DEBUG", message, context);
    }

    static void warn(const std::string& message) {
        log("WARN", message);
    }

    static void warn(const std::string& message, const LogContext& context) {
        log("WARN", message, context);
    }

    static void close() {
        std::lock_guard<std::mutex> lock(write_mutex);
        if (log_file.is_open()) {
            log_file.close();
        }
        initialized = false;
    }
};

// Convenience macros
#define LOG_INFO(msg) velix::utils::Logger::info(msg)
#define LOG_ERROR(msg) velix::utils::Logger::error(msg)
#define LOG_DEBUG(msg) velix::utils::Logger::debug(msg)
#define LOG_WARN(msg) velix::utils::Logger::warn(msg)

// Context-aware convenience macros
#define LOG_INFO_CTX(msg, moduleName, treeId, procId, evt) \
    velix::utils::Logger::info((msg), velix::utils::LogContext{(moduleName), (treeId), (procId), (evt)})

#define LOG_ERROR_CTX(msg, moduleName, treeId, procId, evt) \
    velix::utils::Logger::error((msg), velix::utils::LogContext{(moduleName), (treeId), (procId), (evt)})

#define LOG_DEBUG_CTX(msg, moduleName, treeId, procId, evt) \
    velix::utils::Logger::debug((msg), velix::utils::LogContext{(moduleName), (treeId), (procId), (evt)})

#define LOG_WARN_CTX(msg, moduleName, treeId, procId, evt) \
    velix::utils::Logger::warn((msg), velix::utils::LogContext{(moduleName), (treeId), (procId), (evt)})

} // namespace velix::utils
