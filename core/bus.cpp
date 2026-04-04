#include "bus.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/thread_pool.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>

using json = nlohmann::json;

namespace velix::core {

namespace {

struct BusConfig {
  int max_client_threads = 64;
  int port = 5174;
};

class BusService {
public:
  BusService() : running_(false) {}

  void start(int port_override = -1) {
    if (running_.exchange(true))
      return;

    config_.port = (port_override > 0) ? port_override
                                       : velix::utils::get_port("BUS", 5174);

    try {
      const std::string bind_host =
        velix::communication::resolve_bind_host("BUS", "0.0.0.0");
      server_socket_ = std::make_unique<velix::communication::SocketWrapper>();
      server_socket_->create_tcp_socket();
      server_socket_->bind(bind_host, static_cast<uint16_t>(config_.port));
      server_socket_->listen(128);

      LOG_INFO_CTX("Velix Bus listening on " + bind_host + ":" +
               std::to_string(config_.port),
                   "bus", "BUS_ROOT", -1, "startup");

      while (running_) {
        try {
          velix::communication::SocketWrapper *server_socket = nullptr;
          {
            std::lock_guard<std::mutex> lock(server_mutex_);
            if (!server_socket_ || !server_socket_->is_open()) {
              break;
            }
            server_socket = server_socket_.get();
          }

          if (server_socket == nullptr || !server_socket->is_open()) {
            break;
          }

          if (!server_socket->has_data(250)) {
            continue;
          }

          velix::communication::SocketWrapper client;
          client = server_socket->accept();

          auto client_ptr =
              std::make_shared<velix::communication::SocketWrapper>(
                  std::move(client));
          thread_pool_.try_submit([this, client_ptr]() mutable {
            handle_session(std::move(*client_ptr));
          });
        } catch (const std::exception &e) {
          if (running_) {
            LOG_WARN_CTX("Bus accept error: " + std::string(e.what()), "bus",
                         "", -1, "accept_error");
          }
        }
      }
    } catch (const std::exception &e) {
      running_ = false;
      LOG_ERROR_CTX("Bus critical failure: " + std::string(e.what()), "bus", "",
                    -1, "startup_failure");
    }
  }

  void stop() {
    running_ = false;
    std::lock_guard<std::mutex> lock(server_mutex_);
    if (server_socket_) {
      server_socket_->close();
    }
  }

private:
  void handle_session(velix::communication::SocketWrapper client_sock) {
    int registered_pid = -1;
    auto socket_ptr = std::make_shared<velix::communication::SocketWrapper>(
        std::move(client_sock));

    try {
      while (running_ && socket_ptr->is_open()) {
        std::string raw = velix::communication::recv_json(*socket_ptr);
        json msg = json::parse(raw);

        std::string msg_type = msg.value("message_type", "");

        if (msg_type == "BUS_REGISTER") {
          registered_pid = msg.value("pid", -1);
          if (registered_pid > 0) {
            std::lock_guard<std::mutex> lock(registry_mutex_);
            pid_sockets_[registered_pid] = socket_ptr;
            LOG_INFO_CTX("Process " + std::to_string(registered_pid) +
                             " connected to Bus",
                         "bus", "BUS", registered_pid, "register");
            velix::communication::send_json(*socket_ptr,
                                            json({{"status", "ok"}}).dump());
          }
        } else if (msg_type == "IPM_RELAY") {
          const int target_pid = msg.value("target_pid", -1);
          if (target_pid > 0) {
            relay_message(registered_pid, target_pid, msg);
          }
        }
      }
    } catch (...) {
      // Disconnect handled by cleanup
    }

    if (registered_pid > 0) {
      std::lock_guard<std::mutex> lock(registry_mutex_);
      pid_sockets_.erase(registered_pid);
      delivery_mutexes_.erase(registered_pid);
    }
  }

  void relay_message(int source_pid, int target_pid, const json &msg) {
    std::shared_ptr<velix::communication::SocketWrapper> target_socket;
    std::shared_ptr<std::mutex> target_lock;

    {
      std::lock_guard<std::mutex> lock(registry_mutex_);
      auto it = pid_sockets_.find(target_pid);
      if (it != pid_sockets_.end()) {
        target_socket = it->second;
        // Get or create a delivery mutex for this PID to ensure sequential
        // atomicity
        if (delivery_mutexes_.count(target_pid) == 0) {
          delivery_mutexes_[target_pid] = std::make_shared<std::mutex>();
        }
        target_lock = delivery_mutexes_[target_pid];
      }
    }

    if (target_socket && target_socket->is_open()) {
      try {
        json push_msg = msg;
        push_msg["message_type"] = "IPM_PUSH";
        push_msg["source_pid"] = source_pid;

        // Sequence delivery: lock the per-PID mutex
        std::lock_guard<std::mutex> lock(*target_lock);
        velix::communication::send_json(*target_socket, push_msg.dump());
      } catch (const std::exception &e) {
        LOG_WARN_CTX("Failed to relay message to PID " +
                         std::to_string(target_pid) + ": " + e.what(),
                     "bus", "", source_pid, "relay_fail");
      }
    } else {
      LOG_WARN_CTX("Dropping relay to non-registered PID " +
                       std::to_string(target_pid),
                   "bus", "", source_pid, "relay_target_missing");
    }
  }

  BusConfig config_;
  std::atomic<bool> running_;
  std::mutex server_mutex_;
  std::unique_ptr<velix::communication::SocketWrapper> server_socket_;
  velix::utils::ThreadPool thread_pool_{64, 256};

  std::mutex registry_mutex_;
  std::unordered_map<int, std::shared_ptr<velix::communication::SocketWrapper>>
      pid_sockets_;
  std::unordered_map<int, std::shared_ptr<std::mutex>> delivery_mutexes_;
};

BusService &bus_instance() {
  static BusService service;
  return service;
}

} // namespace

void start_bus(int port) { bus_instance().start(port); }
void stop_bus() { bus_instance().stop(); }

} // namespace velix::core
