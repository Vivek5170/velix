#ifndef VELIX_THREAD_POOL_HPP
#define VELIX_THREAD_POOL_HPP

#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace velix {
namespace utils {

/**
 * Fixed-size thread pool for handling asynchronous connections.
 * Uses only standard C++11 primitives — works on Linux, macOS, and Windows.
 * Worker threads are created once at construction and reused, eliminating
 * per-connection thread create/destroy bounds.
 */
class ThreadPool {
public:
  explicit ThreadPool(int thread_count, int max_queued)
      : stop_(false), pending_count_(0),
        capacity_(static_cast<std::size_t>(max_queued > 0 ? max_queued : 512)) {
    workers_.reserve(static_cast<std::size_t>(thread_count));
    for (int i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { worker_loop(); });
    }
  }

  ~ThreadPool() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto &w : workers_) {
      if (w.joinable()) {
        w.join();
      }
    }
  }

  // Non-copyable, non-movable
  ThreadPool(const ThreadPool &) = delete;
  ThreadPool &operator=(const ThreadPool &) = delete;

  /**
   * Try to submit a task. Returns false if the pool is at capacity
   * (back-pressure). Safe to call from any thread.
   */
  bool try_submit(std::function<void()> task) {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (stop_ || pending_count_ >= capacity_) {
        return false;
      }
      tasks_.push(std::move(task));
      ++pending_count_;
    }
    cv_.notify_one();
    return true;
  }

private:
  void worker_loop() {
    while (true) {
      std::function<void()> task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return stop_ || !tasks_.empty(); });
        if (stop_ && tasks_.empty()) {
          return;
        }
        task = std::move(tasks_.front());
        tasks_.pop();
        --pending_count_;
      }
      task();
    }
  }

  std::vector<std::thread> workers_;
  std::queue<std::function<void()>> tasks_;
  std::mutex mutex_;
  std::condition_variable cv_;
  bool stop_;
  std::size_t pending_count_;
  const std::size_t capacity_;
};

} // namespace utils
} // namespace velix

#endif // VELIX_THREAD_POOL_HPP
