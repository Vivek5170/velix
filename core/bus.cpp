#include "bus.hpp"

#include "../communication/asio_framing.hpp"
#include "../communication/json_include.hpp"
#include "../communication/network_config.hpp"
#include "../utils/asio_runtime.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"

#include <asio.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>

using json = nlohmann::json;

namespace velix::core {

namespace {

using asio::ip::tcp;

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
  int io_threads = 4;
  int port = 5174;
  std::uint32_t max_message_bytes = 10U * 1024U * 1024U;
};

class BusService;

class BusSession : public std::enable_shared_from_this<BusSession> {
public:
  BusSession(BusService &service, tcp::socket socket);

  void start();
  void enqueue_json(std::string payload);
  void close();

  int registered_pid() const { return registered_pid_; }
  const std::string &registered_tree_id() const { return registered_tree_id_; }

private:
  void read_next();
  void handle_payload(std::string payload);
  void write_next();
  void close_on_strand();

  BusService &service_;
  tcp::socket socket_;
  asio::strand<asio::any_io_executor> strand_;
  std::deque<std::string> write_queue_;
  int registered_pid_{-1};
  std::string registered_tree_id_;
  bool registered_tree_root_{false};
  bool closed_{false};
};

class BusService {
public:
  BusService() : acceptor_(runtime_.context()) {}

  void start(int port_override = -1) {
    if (running_.exchange(true)) {
      return;
    }

    config_.port = (port_override > 0) ? port_override
                                       : velix::utils::get_port("BUS", 5174);
    config_.io_threads = static_cast<int>(
        velix::utils::get_config("VELIX_BUS_IO_THREADS", config_.io_threads));
    config_.max_message_bytes = static_cast<std::uint32_t>(
        velix::utils::get_config("VELIX_COMM_MAX_MESSAGE_SIZE",
                                 config_.max_message_bytes));

    try {
      const std::string bind_host =
          velix::communication::resolve_bind_host("BUS", "127.0.0.1");
      const tcp::endpoint endpoint(asio::ip::make_address(bind_host),
                                   static_cast<unsigned short>(config_.port));

      acceptor_.open(endpoint.protocol());
      acceptor_.set_option(asio::socket_base::reuse_address(true));
      acceptor_.bind(endpoint);
      acceptor_.listen(asio::socket_base::max_listen_connections);

      LOG_INFO_CTX("Velix Bus listening on " + bind_host + ":" +
                       std::to_string(config_.port),
                   "bus", "BUS_ROOT", -1, "startup");

      accept_next();
      runtime_.start(static_cast<std::size_t>(config_.io_threads));

      std::unique_lock<std::mutex> lock(lifecycle_mutex_);
      lifecycle_cv_.wait(lock, [this] { return !running_.load(); });
    } catch (const std::exception &e) {
      running_ = false;
      LOG_ERROR_CTX("Bus critical failure: " + std::string(e.what()), "bus", "",
                    -1, "startup_failure");
    }
  }

  void stop() {
    if (!running_.exchange(false)) {
      return;
    }

    asio::error_code ignored;
    acceptor_.cancel(ignored);
    acceptor_.close(ignored);

    std::unordered_map<int, std::shared_ptr<BusSession>> sessions;
    {
      std::scoped_lock lock(registry_mutex_);
      sessions = pid_sessions_;
      pid_sessions_.clear();
      tree_root_pid_map_.clear();
    }

    for (auto &[_, session] : sessions) {
      if (session) {
        session->close();
      }
    }

    runtime_.stop();
    lifecycle_cv_.notify_all();
  }

  std::uint32_t max_message_bytes() const { return config_.max_message_bytes; }
  asio::io_context &io_context() { return runtime_.context(); }

  void handle_message(const std::shared_ptr<BusSession> &session,
                      const json &msg) {
    const std::string msg_type = msg.value("message_type", "");

    if (msg_type == "BUS_REGISTER") {
      register_session(session, msg);
      return;
    }

    if (msg_type == "IPM_RELAY") {
      const int requested_target_pid = msg.value("target_pid", -1);
      const int target_pid = resolve_target_pid(msg, requested_target_pid);
      if (target_pid > 0) {
        relay_message(session->registered_pid(), target_pid, msg);
      } else {
        LOG_WARN_CTX("Dropping relay due to unresolved target_pid=" +
                         std::to_string(requested_target_pid),
                     "bus", "", session->registered_pid(),
                     "relay_target_missing");
      }
      return;
    }

    if (msg_type == "HEARTBEAT") {
      forward_heartbeat_to_supervisor(session, msg);
    }
  }

  void unregister_session(const std::shared_ptr<BusSession> &session, int pid,
                          const std::string &tree_id) {
    if (pid <= 0) {
      return;
    }

    std::scoped_lock lock(registry_mutex_);
    if (const auto it = pid_sessions_.find(pid);
        it != pid_sessions_.end() && it->second == session) {
      pid_sessions_.erase(it);
    }

    if (!tree_id.empty()) {
      if (const auto it = tree_root_pid_map_.find(tree_id);
          it != tree_root_pid_map_.end() && it->second == pid) {
        tree_root_pid_map_.erase(it);
      }
    }
  }

private:
  void accept_next() {
    if (!running_) {
      return;
    }

    acceptor_.async_accept(
        asio::make_strand(runtime_.context()),
        [this](const asio::error_code &ec, tcp::socket socket) {
          if (!running_) {
            return;
          }

          if (ec) {
            if (ec != asio::error::operation_aborted) {
              LOG_WARN_CTX("Bus accept error: " + ec.message(), "bus", "", -1,
                           "accept_error");
            }
          } else {
            std::make_shared<BusSession>(*this, std::move(socket))->start();
          }

          accept_next();
        });
  }

  void register_session(const std::shared_ptr<BusSession> &session,
                        const json &msg) {
    const int registered_pid = msg.value("pid", -1);
    if (registered_pid <= 0) {
      return;
    }

    {
      std::scoped_lock lock(registry_mutex_);
      pid_sessions_[registered_pid] = session;
      const std::string tree_id = msg.value("tree_id", std::string(""));
      if (msg.value("is_root", false) && !tree_id.empty()) {
        tree_root_pid_map_[tree_id] = registered_pid;
      }
    }

    LOG_INFO_CTX("Process " + std::to_string(registered_pid) +
                     " connected to Bus",
                 "bus", "BUS", registered_pid, "register");
    session->enqueue_json(json({{"status", "ok"}}).dump());
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

  void relay_message(int source_pid, int target_pid, const json &msg) {
    std::shared_ptr<BusSession> target_session;
    {
      std::scoped_lock lock(registry_mutex_);
      if (const auto it = pid_sessions_.find(target_pid);
          it != pid_sessions_.end()) {
        target_session = it->second;
      }
    }

    if (target_session) {
      json push_msg = msg;
      push_msg["message_type"] = "IPM_PUSH";
      push_msg["source_pid"] = source_pid;
      target_session->enqueue_json(push_msg.dump());
    } else {
      LOG_WARN_CTX("Dropping relay to non-registered PID " +
                       std::to_string(target_pid),
                   "bus", "", source_pid, "relay_target_missing");
    }
  }

  void forward_heartbeat_to_supervisor(
      const std::shared_ptr<BusSession> &client_session, const json &heartbeat) {
    const int sup_port = velix::utils::get_port("SUPERVISOR", 5173);
    const std::string sup_host =
        velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1");

    auto supervisor_socket = std::make_shared<tcp::socket>(runtime_.context());
    auto request_frame = std::make_shared<std::string>(
        velix::communication::make_frame(heartbeat.dump()));

    try {
      const tcp::endpoint endpoint(asio::ip::make_address(sup_host),
                                   static_cast<unsigned short>(sup_port));
      supervisor_socket->async_connect(
          endpoint, [this, supervisor_socket, request_frame, client_session,
                     heartbeat](const asio::error_code &connect_ec) {
            if (connect_ec) {
              send_supervisor_unreachable(client_session, heartbeat, connect_ec);
              return;
            }

            asio::async_write(
                *supervisor_socket, asio::buffer(*request_frame),
                [this, supervisor_socket, client_session, heartbeat](
                    const asio::error_code &write_ec, std::size_t) {
                  if (write_ec) {
                    send_supervisor_unreachable(client_session, heartbeat,
                                                write_ec);
                    return;
                  }

                  velix::communication::async_read_frame(
                      *supervisor_socket, config_.max_message_bytes,
                      [this, supervisor_socket, client_session, heartbeat](
                          const asio::error_code &read_ec,
                          std::string payload) {
                        if (read_ec) {
                          send_supervisor_unreachable(client_session, heartbeat,
                                                      read_ec);
                          return;
                        }

                        client_session->enqueue_json(std::move(payload));
                        asio::error_code ignored;
                        supervisor_socket->close(ignored);
                      });
                });
          });
    } catch (const std::exception &e) {
      LOG_WARN_CTX(std::string("Bus failed to start heartbeat forward: ") +
                       e.what(),
                   "bus", "", heartbeat.value("pid", -1),
                   "heartbeat_forward_fail");
      client_session->enqueue_json(
          json({{"status", "error"}, {"error", "supervisor_unreachable"}})
              .dump());
    }
  }

  void send_supervisor_unreachable(
      const std::shared_ptr<BusSession> &client_session, const json &heartbeat,
      const asio::error_code &ec) {
    LOG_WARN_CTX(std::string("Bus failed to forward heartbeat to Supervisor: ") +
                     ec.message(),
                 "bus", "", heartbeat.value("pid", -1),
                 "heartbeat_forward_fail");
    client_session->enqueue_json(
        json({{"status", "error"}, {"error", "supervisor_unreachable"}})
            .dump());
  }

  BusConfig config_;
  std::atomic<bool> running_{false};
  velix::utils::AsioRuntime runtime_;
  tcp::acceptor acceptor_;
  std::mutex lifecycle_mutex_;
  std::condition_variable lifecycle_cv_;

  std::mutex registry_mutex_;
  std::unordered_map<int, std::shared_ptr<BusSession>> pid_sessions_;
  std::unordered_map<std::string, int, TransparentStringHash,
                     TransparentStringEqual>
      tree_root_pid_map_;
};

BusSession::BusSession(BusService &service, tcp::socket socket)
    : service_(service), socket_(std::move(socket)),
      strand_(asio::make_strand(socket_.get_executor())) {}

void BusSession::start() { read_next(); }

void BusSession::enqueue_json(std::string payload) {
  auto self = shared_from_this();
  asio::post(strand_, [this, self, frame = velix::communication::make_frame(
                                     std::move(payload))]() mutable {
    const bool write_in_progress = !write_queue_.empty();
    write_queue_.push_back(std::move(frame));
    if (!write_in_progress) {
      write_next();
    }
  });
}

void BusSession::close() {
  auto self = shared_from_this();
  asio::post(strand_, [this, self] { close_on_strand(); });
}

void BusSession::read_next() {
  auto self = shared_from_this();
  velix::communication::async_read_frame(
      socket_, service_.max_message_bytes(),
      asio::bind_executor(
          strand_, [this, self](const asio::error_code &ec,
                                std::string payload) {
            if (ec) {
              close_on_strand();
              return;
            }

            handle_payload(std::move(payload));
            if (!closed_) {
              read_next();
            }
          }));
}

void BusSession::handle_payload(std::string payload) {
  try {
    json msg = json::parse(payload);
    const std::string msg_type = msg.value("message_type", "");
    if (msg_type == "BUS_REGISTER") {
      registered_pid_ = msg.value("pid", -1);
      registered_tree_id_ = msg.value("tree_id", std::string(""));
      registered_tree_root_ = msg.value("is_root", false);
    }
    service_.handle_message(shared_from_this(), msg);
  } catch (const std::exception &e) {
    LOG_WARN_CTX("Bus session received invalid message: " + std::string(e.what()),
                 "bus", "", registered_pid_, "invalid_message");
    close_on_strand();
  }
}

void BusSession::write_next() {
  if (write_queue_.empty() || closed_) {
    return;
  }

  auto self = shared_from_this();
  asio::async_write(
      socket_, asio::buffer(write_queue_.front()),
      asio::bind_executor(
          strand_, [this, self](const asio::error_code &ec, std::size_t) {
            if (ec) {
              close_on_strand();
              return;
            }

            write_queue_.pop_front();
            write_next();
          }));
}

void BusSession::close_on_strand() {
  if (closed_) {
    return;
  }
  closed_ = true;

  asio::error_code ignored;
  socket_.close(ignored);
  write_queue_.clear();
  service_.unregister_session(shared_from_this(), registered_pid_,
                              registered_tree_id_);
}

BusService &bus_instance() {
  static BusService service;
  return service;
}

} // namespace

void start_bus(int port) { bus_instance().start(port); }
void stop_bus() { bus_instance().stop(); }

} // namespace velix::core
