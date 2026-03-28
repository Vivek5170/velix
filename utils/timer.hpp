#pragma once

#include <chrono>
#include <string>

namespace velix::utils {

class Timer {
private:
    std::chrono::high_resolution_clock::time_point start_time;
    std::chrono::high_resolution_clock::time_point end_time;
    bool running;

public:
    Timer() : running(false) {
        reset();
    }

    void start() {
        start_time = std::chrono::high_resolution_clock::now();
        running = true;
    }

    void stop() {
        end_time = std::chrono::high_resolution_clock::now();
        running = false;
    }

    // Returns elapsed time in milliseconds
    long long elapsed_ms() const {
        auto end = running ? std::chrono::high_resolution_clock::now() : end_time;
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            end - start_time).count();
    }

    // Returns elapsed time in microseconds
    long long elapsed_us() const {
        auto end = running ? std::chrono::high_resolution_clock::now() : end_time;
        return std::chrono::duration_cast<std::chrono::microseconds>(
            end - start_time).count();
    }

    // Returns elapsed time in seconds
    double elapsed_sec() const {
        return elapsed_ms() / 1000.0;
    }

    // Reset timer
    void reset() {
        start_time = std::chrono::high_resolution_clock::now();
        end_time = start_time;
        running = false;
    }

    // Check if timer is running
    bool is_running() const {
        return running;
    }

    // Reset and start immediately
    void restart() {
        start();
    }
};

// RAII-based scoped timer for automatic timing
class ScopedTimer {
private:
    Timer timer;
    std::string name;

public:
    explicit ScopedTimer(const std::string& timer_name) : name(timer_name) {
        timer.start();
    }

    ~ScopedTimer() {
        timer.stop();
        // Note: If logger is needed, include logger.hpp and use LOG_DEBUG
        // For now, just measure silently
    }

    long long elapsed_ms() const {
        return timer.elapsed_ms();
    }

    long long elapsed_us() const {
        return timer.elapsed_us();
    }

    double elapsed_sec() const {
        return timer.elapsed_sec();
    }

    std::string get_name() const {
        return name;
    }
};

} // namespace velix::utils
