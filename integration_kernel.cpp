#include "./communication/network_config.hpp"
#include "./communication/socket_wrapper.hpp"
#include "./core/bus.hpp"
#include "./core/persistant_applications/application_manager.hpp"
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

std::atomic<bool> &shutdown_requested() {
  static std::atomic requested{false};
  return requested;
}

void handle_signal(int) { shutdown_requested().store(true); }

void request_shutdown_once() {
  static std::atomic already_called{false};
  if (const bool was_already_called = already_called.exchange(true);
      was_already_called) {
    return;
  }

  LOG_INFO("Integration kernel shutdown requested");
  velix::core::stop_supervisor();
  velix::llm::stop_scheduler();
  velix::app_manager::stop_application_manager();
  velix::core::stop_executioner();
  velix::core::stop_bus();
}

bool wait_for_service(const std::string &service_name, int port,
                      int timeout_ms = 10000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  const std::string host =
      velix::communication::resolve_service_host(service_name, "127.0.0.1");

  while (!shutdown_requested().load()) {
    try {
      velix::communication::SocketWrapper probe;
      probe.create_tcp_socket();
      probe.connect(host, static_cast<uint16_t>(port));
      probe.close();
      return true;
    } catch (const std::exception &) {
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
  const int app_manager_port = velix::utils::get_port("APP_MANAGER", 5175);

  LOG_INFO("Integration kernel starting...");
  LOG_INFO("Supervisor Port: " + std::to_string(supervisor_port));
  LOG_INFO("Bus Port: " + std::to_string(bus_port));
  LOG_INFO("Executioner Port: " + std::to_string(executioner_port));
  LOG_INFO("Scheduler Port: " + std::to_string(scheduler_port));
  LOG_INFO("ApplicationManager Port: " + std::to_string(app_manager_port));

  std::thread bus_thread([bus_port]() { velix::core::start_bus(bus_port); });
  std::thread executioner_thread([executioner_port]() {
    try {
      velix::core::start_executioner(executioner_port);
    } catch (const std::exception &e) {
      if (!shutdown_requested().load()) {
        LOG_ERROR("Executioner thread failed: " + std::string(e.what()));
      }
    }
  });
  std::thread scheduler_thread(
      [scheduler_port]() { velix::llm::start_scheduler(scheduler_port); });
  std::thread app_manager_thread(
      [app_manager_port]() { velix::app_manager::start_application_manager(app_manager_port); });

  if (!wait_for_service("BUS", bus_port) ||
      !wait_for_service("EXECUTIONER", executioner_port) ||
      !wait_for_service("SCHEDULER", scheduler_port) ||
      !wait_for_service("APP_MANAGER", app_manager_port)) {
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
    if (app_manager_thread.joinable()) {
      app_manager_thread.join();
    }
    return 1;
  }

  std::thread signal_watcher([]() {
    while (!shutdown_requested().load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    request_shutdown_once();
  });

  try {
    velix::core::start_supervisor(supervisor_port);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Supervisor thread failed: ") + e.what());
    if (!shutdown_requested().load()) {
      request_shutdown_once();
    }
  }

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
  if (app_manager_thread.joinable()) {
    app_manager_thread.join();
  }

  return 0;
}
