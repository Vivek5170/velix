#pragma once

#include <asio.hpp>

#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

namespace velix::utils {

class AsioRuntime {
public:
  AsioRuntime()
      : work_guard_(std::make_unique<WorkGuard>(asio::make_work_guard(io_))) {}

  ~AsioRuntime() noexcept { try { stop(); } catch (...) { /* shut down cleanly */ } }

  AsioRuntime(const AsioRuntime &) = delete;
  AsioRuntime &operator=(const AsioRuntime &) = delete;

  asio::io_context &context() { return io_; }

  void start(std::size_t thread_count) {
    if (!threads_.empty()) {
      return;
    }
    if (thread_count == 0) {
      thread_count = 1;
    }

    io_.restart();
    if (!work_guard_) {
      work_guard_ = std::make_unique<WorkGuard>(asio::make_work_guard(io_));
    }

    threads_.reserve(thread_count);
    for (std::size_t i = 0; i < thread_count; ++i) {
      threads_.emplace_back([this] { io_.run(); });
    }
  }

  void stop() {
    if (work_guard_) {
      work_guard_->reset();
    }
    io_.stop();

    for (auto &thread : threads_) {
      if (thread.joinable()) {
        thread.join();
      }
    }
    threads_.clear();
  }

private:
  using WorkGuard = asio::executor_work_guard<asio::io_context::executor_type>;

  asio::io_context io_;
  std::unique_ptr<WorkGuard> work_guard_;
  std::vector<std::thread> threads_;
};

} // namespace velix::utils
