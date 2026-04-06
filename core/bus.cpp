#include "bus.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/thread_pool.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

using json = nlohmann::json;

namespace velix::core {

namespace {

struct TransparentStringHash {
  using is_transparent = void;

  std::size_t operator()(std::string_view value) const noexcept {
    return std::hash<std::string_view>{}(value);
  }

  std::size_t operator()(const std::string &value) const noexcept {
    return operator()(std::string_view(value));
  }

  std::size_t operator()(const char *value) const noexcept {
    return operator()(std::string_view(value));
  }
};

struct TransparentStringEqual {
  using is_transparent = void;

  bool operator()(std::string_view lhs, std::string_view rhs) const noexcept {
    return lhs == rhs;
  }
};

struct BusConfig {
  int max_client_threads = 64;
  int port = 5174;
};

class BusService {
public:
  BusService() = default;

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
        if (!accept_and_dispatch_once()) {
          break;
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

    {
      std::scoped_lock lock(server_mutex_);
      if (server_socket_) {
        server_socket_->close();
      }
    }

    {
      std::scoped_lock lock(registry_mutex_);
      for (auto &entry : pid_sockets_) {
        if (entry.second && entry.second->is_open()) {
          entry.second->close();
        }
      }
      pid_sockets_.clear();
      delivery_mutexes_.clear();
      tree_root_pid_map_.clear();
    }
  }

private:
  bool accept_and_dispatch_once() {
    try {
      velix::communication::SocketWrapper *server_socket = nullptr;
      {
        std::scoped_lock lock(server_mutex_);
        if (server_socket_ && server_socket_->is_open()) {
          server_socket = server_socket_.get();
        }
      }

      if (server_socket == nullptr || !server_socket->is_open()) {
        return false;
      }

      if (!server_socket->has_data(250)) {
        return true;
      }

      velix::communication::SocketWrapper client = server_socket->accept();
      auto client_ptr = std::make_shared<velix::communication::SocketWrapper>();
      *client_ptr = std::move(client);
      thread_pool_.try_submit([this, client_ptr]() mutable {
        handle_session(std::move(*client_ptr));
      });
      return true;
    } catch (const std::exception &e) {
      if (running_) {
        LOG_WARN_CTX("Bus accept error: " + std::string(e.what()), "bus", "",
                     -1, "accept_error");
      }
      return running_.load();
    }
  }

  int resolve_target_pid(const json &msg, int requested_target_pid) {
    if (requested_target_pid != -1) {
      return requested_target_pid;
    }

    std::scoped_lock lock(registry_mutex_);
    if (const auto handler_it = tree_root_pid_map_.find("TREE_HANDLER");
        handler_it != tree_root_pid_map_.end()) {
      return handler_it->second;
    }

    const std::string tree_id = msg.value("tree_id", std::string(""));
    if (const auto tree_it = tree_root_pid_map_.find(tree_id);
        tree_it != tree_root_pid_map_.end()) {
      return tree_it->second;
    }

    return -1;
  }

  void handle_session(velix::communication::SocketWrapper client_sock) {
    int registered_pid = -1;
    std::string registered_tree_id;
    bool registered_tree_root = false;
    auto socket_ptr = std::make_shared<velix::communication::SocketWrapper>();
    *socket_ptr = std::move(client_sock);

    try {
      while (running_ && socket_ptr->is_open()) {
        std::string raw = velix::communication::recv_json(*socket_ptr);
        json msg = json::parse(raw);

        std::string msg_type = msg.value("message_type", "");

        if (msg_type == "BUS_REGISTER") {
          registered_pid = msg.value("pid", -1);
          registered_tree_id = msg.value("tree_id", std::string(""));
          registered_tree_root = msg.value("is_root", false);
          if (registered_pid > 0) {
            std::scoped_lock lock(registry_mutex_);
            pid_sockets_[registered_pid] = socket_ptr;
            if (registered_tree_root && !registered_tree_id.empty()) {
              tree_root_pid_map_[registered_tree_id] = registered_pid;
            }
            LOG_INFO_CTX("Process " + std::to_string(registered_pid) +
                             " connected to Bus",
                         "bus", "BUS", registered_pid, "register");
            velix::communication::send_json(*socket_ptr,
                                            json({{"status", "ok"}}).dump());
          }
        } else if (msg_type == "IPM_RELAY") {
          const int requested_target_pid = msg.value("target_pid", -1);
          const int target_pid = resolve_target_pid(msg, requested_target_pid);
          if (target_pid > 0) {
            relay_message(registered_pid, target_pid, msg);
          } else {
            LOG_WARN_CTX("Dropping relay due to unresolved target_pid=" +
                             std::to_string(requested_target_pid),
                         "bus", "", registered_pid, "relay_target_missing");
          }
        }
      }
    } catch (const std::exception &) {
      // Disconnect handled by cleanup.
    }

    if (registered_pid > 0) {
      std::scoped_lock lock(registry_mutex_);
      pid_sockets_.erase(registered_pid);
      delivery_mutexes_.erase(registered_pid);
      if (!registered_tree_id.empty()) {
        if (const auto it = tree_root_pid_map_.find(registered_tree_id);
            it != tree_root_pid_map_.end() && it->second == registered_pid) {
          tree_root_pid_map_.erase(it);
        }
      }
    }
  }

  void relay_message(int source_pid, int target_pid, const json &msg) {
    std::shared_ptr<velix::communication::SocketWrapper> target_socket;
    std::shared_ptr<std::mutex> target_lock;

    {
      std::scoped_lock lock(registry_mutex_);
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
        std::scoped_lock lock(*target_lock);
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
  std::atomic<bool> running_{false};
  std::mutex server_mutex_;
  std::unique_ptr<velix::communication::SocketWrapper> server_socket_;
  velix::utils::ThreadPool thread_pool_{64, 256};

  std::mutex registry_mutex_;
  std::unordered_map<int, std::shared_ptr<velix::communication::SocketWrapper>>
      pid_sockets_;
  std::unordered_map<int, std::shared_ptr<std::mutex>> delivery_mutexes_;
  std::unordered_map<std::string, int, TransparentStringHash,
                     TransparentStringEqual>
      tree_root_pid_map_;
};

BusService &bus_instance() {
  static BusService service;
  return service;
}

} // namespace

void start_bus(int port) { bus_instance().start(port); }
void stop_bus() { bus_instance().stop(); }

} // namespace velix::core
