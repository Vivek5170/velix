#include "./communication/network_config.hpp"
#include "./communication/socket_wrapper.hpp"
#include "./core/bus.hpp"
#include "./core/supervisor.hpp"
#include "./execution/executioner.hpp"
#include "./llm/scheduler.hpp"
#include "./utils/config_utils.hpp"
#include "./utils/logger.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <thread>

namespace {

std::atomic<bool> g_shutdown_requested{false};

void handle_signal(int) { g_shutdown_requested.store(true); }

void request_shutdown_once() {
  static std::atomic<bool> already_called{false};
  if (already_called.exchange(true)) {
    return;
  }

  LOG_INFO("Integration kernel shutdown requested");
  velix::core::stop_supervisor();
  velix::llm::stop_scheduler();
  velix::core::stop_executioner();
  velix::core::stop_bus();
}

bool wait_for_service(const std::string &service_name, int port,
                      int timeout_ms = 10000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  const std::string host =
      velix::communication::resolve_service_host(service_name, "127.0.0.1");

  while (!g_shutdown_requested.load()) {
    try {
      velix::communication::SocketWrapper probe;
      probe.create_tcp_socket();
      probe.connect(host, static_cast<uint16_t>(port));
      probe.close();
      return true;
    } catch (...) {
      if (std::chrono::steady_clock::now() >= deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
  }

  return false;
}

} // namespace

int main() {
  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  const int supervisor_port = velix::utils::get_port("SUPERVISOR", 5173);
  const int bus_port = velix::utils::get_port("BUS", 5174);
  const int executioner_port = velix::utils::get_port("EXECUTIONER", 5172);
  const int scheduler_port = velix::utils::get_port("LLM_SCHEDULER", 5171);

  LOG_INFO("Integration kernel starting...");
  LOG_INFO("Supervisor Port: " + std::to_string(supervisor_port));
  LOG_INFO("Bus Port: " + std::to_string(bus_port));
  LOG_INFO("Executioner Port: " + std::to_string(executioner_port));
  LOG_INFO("Scheduler Port: " + std::to_string(scheduler_port));

  std::thread bus_thread([bus_port]() { velix::core::start_bus(bus_port); });
  std::thread executioner_thread([executioner_port]() {
    velix::core::start_executioner(executioner_port);
  });
  std::thread scheduler_thread(
      [scheduler_port]() { velix::llm::start_scheduler(scheduler_port); });

  if (!wait_for_service("BUS", bus_port) ||
      !wait_for_service("EXECUTIONER", executioner_port) ||
      !wait_for_service("SCHEDULER", scheduler_port)) {
    LOG_ERROR(
        "Integration kernel startup failed: timed out waiting for services");
    request_shutdown_once();

    if (bus_thread.joinable()) {
      bus_thread.join();
    }
    if (executioner_thread.joinable()) {
      executioner_thread.join();
    }
    if (scheduler_thread.joinable()) {
      scheduler_thread.join();
    }
    return 1;
  }

  std::thread signal_watcher([]() {
    while (!g_shutdown_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    request_shutdown_once();
  });

  velix::core::start_supervisor(supervisor_port);

  request_shutdown_once();

  if (signal_watcher.joinable()) {
    signal_watcher.join();
  }

  if (bus_thread.joinable()) {
    bus_thread.join();
  }
  if (executioner_thread.joinable()) {
    executioner_thread.join();
  }
  if (scheduler_thread.joinable()) {
    scheduler_thread.join();
  }

  return 0;
}
