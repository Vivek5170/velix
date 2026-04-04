#include "../runtime/sdk/cpp/velix_process.hpp"
#include <iostream>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <unordered_map>

using namespace velix::core;
using namespace velix::communication;

class Handler : public VelixProcess {
public:
  Handler() : VelixProcess("terminal_handler", "handler") {}

  void on_shutdown() override {
    std::lock_guard<std::mutex> lock(terminal_client_mutex);
    if (terminal_client.is_open()) {
      try {
        terminal_client.close();
      } catch (const std::exception &e) {
        std::cerr << "[Handler] Error closing terminal_client: " << e.what()
                  << std::endl;
      }
    }
    if (server.is_open()) {
      try {
        server.close();
      } catch (const std::exception &e) {
        std::cerr << "[Handler] Error closing server: " << e.what()
                  << std::endl;
      }
    }
  }

  void run() override {
    auto send_terminal_event = [&](const json &event) {
      std::lock_guard<std::mutex> lock(terminal_client_mutex);
      if (terminal_client.is_open()) {
        try {
          send_json(terminal_client, event.dump());
        } catch (const std::exception &) {
          // Ignore client disconnects while reporting events.
        }
      }
    };

    on_tool_start = [&](const std::string &instruction, const json &args) {
      json event = {
          {"type", "tool_start"},
          {"tool", instruction},
          {"args", args}};
      send_terminal_event(event);
      std::cout << "[Handler] Tool start: " << instruction << std::endl;
    };

    on_tool_finish = [&](const std::string &instruction, const json &result) {
      json event = {
          {"type", "tool_finish"},
          {"tool", instruction},
          {"result", result}};
      send_terminal_event(event);
      std::cout << "[Handler] Tool finish: " << instruction << std::endl;
    };

    on_bus_event = [&](const json &msg) {
      std::string purpose = msg.value("purpose", "");
      int source_pid = msg.value("source_pid", -1);
      json payload = msg.value("payload", json::object());
      std::string sender_user_id = msg.value("user_id", "");

      if (purpose == "APPROVAL_REQUEST") {
        const std::string approval_trace =
            payload.value("approval_trace", std::string(""));
        const std::string command =
            payload.value("command", std::string(""));
        const std::string description =
            payload.value("description", std::string(""));

        std::cout << "[Handler][Approval][Request] trace=" << approval_trace
                  << " source_pid=" << source_pid
                  << " command=\"" << command << "\""
                  << " description=\"" << description << "\""
                  << std::endl;

        if (!approval_trace.empty() && source_pid > 0) {
          std::lock_guard<std::mutex> lock(approval_map_mutex);
          approval_trace_to_pid[approval_trace] = source_pid;
          std::cout << "[Handler][Approval] mapped trace=" << approval_trace
                    << " -> pid=" << source_pid << std::endl;
        } else {
          std::cout << "[Handler][Approval][Request] invalid mapping data"
                    << " (trace empty or source pid invalid)" << std::endl;
        }
      }

      json event = {
          {"type", "notify"},
          {"purpose", purpose},
          {"source_pid", source_pid},
          {"user_id", sender_user_id},
          {"payload", payload}};
      send_terminal_event(event);
      std::cout << "[Handler] Bus event received: purpose=" << purpose
                << " user_id=" << sender_user_id << std::endl;
    };

    try {
      server.create_tcp_socket();
      server.bind("0.0.0.0", 6060);
      server.listen(5);
      std::cout
          << "[Handler] Streaming terminal server listening on port 6060..."
          << std::endl;

      while (is_running) {
        try {
          SocketWrapper accepted_client = server.accept();
          {
            std::lock_guard<std::mutex> lock(terminal_client_mutex);
            terminal_client = std::move(accepted_client);
            try {
              terminal_client.set_timeout_ms(500);
            } catch (const std::exception &) {
              // Best-effort timeout setup; continue without it.
            }
          }

          {
            std::lock_guard<std::mutex> lock(request_queue_mutex);
            while (!request_queue.empty()) {
              request_queue.pop();
            }
          }
          client_active = true;
          std::cout << "[Handler] Terminal client connected." << std::endl;

          std::thread llm_worker([&]() {
            while (is_running && client_active) {
              PendingRequest req;
              {
                std::unique_lock<std::mutex> lock(request_queue_mutex);
                request_queue_cv.wait(lock, [&]() {
                  return !is_running || !client_active || !request_queue.empty();
                });
                if (!is_running || !client_active) {
                  return;
                }
                req = request_queue.front();
                request_queue.pop();
              }
              process_user_message(req);
            }
          });

          while (is_running && client_active) {
            try {
              std::string request_json = recv_json(terminal_client);
              if (request_json.empty()) {
                break;
              }

              auto j = json::parse(request_json);

              if (j.value("type", std::string("")) == "approval_reply") {
                handle_approval_reply(j);
                continue;
              }

              if (!j.contains("message")) {
                continue;
              }

              PendingRequest req;
              req.message = j["message"].is_string() ? j["message"].get<std::string>() : std::string("");
              req.user_id =
                  (j.contains("user_id") && j["user_id"].is_string() &&
                   !j["user_id"].get<std::string>().empty())
                      ? j["user_id"].get<std::string>()
                      : "terminal_cli_user";

              if (req.message.empty()) {
                continue;
              }

              std::cout << "[Handler] Received: " << req.message << std::endl;
              {
                std::lock_guard<std::mutex> lock(request_queue_mutex);
                request_queue.push(std::move(req));
              }
              request_queue_cv.notify_one();
            } catch (const velix::communication::SocketTimeoutException &) {
              continue;
            } catch (const std::exception &request_error) {
              std::cerr << "[Handler] Request error: " << request_error.what() << std::endl;
              break;
            }
          }

          client_active = false;
          request_queue_cv.notify_all();
          if (llm_worker.joinable()) {
            llm_worker.join();
          }

          {
            std::lock_guard<std::mutex> lock(terminal_client_mutex);
            terminal_client.close();
          }
        } catch (const std::exception &e) {
          std::cerr << "[Handler] Client connection error: " << e.what()
                    << std::endl;
        }
      }
      std::cout << "[Handler] Loop exited. is_running=" << is_running << std::endl;
    } catch (const std::exception &e) {
      std::cerr << "[Handler] Critical server error: " << e.what() << std::endl;
    }
  }

private:
  struct PendingRequest {
    std::string message;
    std::string user_id;
  };

  void handle_approval_reply(const json &j) {
    const std::string approval_trace =
        j.value("approval_trace", std::string(""));
    const std::string scope = j.value("scope", std::string("deny"));

    int target_pid = -1;
    {
      std::lock_guard<std::mutex> lock(approval_map_mutex);
      auto it = approval_trace_to_pid.find(approval_trace);
      if (it != approval_trace_to_pid.end()) {
        target_pid = it->second;
        approval_trace_to_pid.erase(it);
      }
    }

    if (target_pid > 0 && !approval_trace.empty()) {
      std::cout << "[Handler][Approval] forwarding trace=" << approval_trace
                << " scope=" << scope << " to pid=" << target_pid
                << std::endl;
      send_message(target_pid, "APPROVAL_REPLY",
                   {{"approval_trace", approval_trace},
                    {"scope", scope}});
    } else {
      std::cout << "[Handler][Approval] no target pid for trace="
                << approval_trace << " scope=" << scope << std::endl;
    }

    std::lock_guard<std::mutex> lock(terminal_client_mutex);
    if (terminal_client.is_open()) {
      json ack = {{"type", "approval_ack"},
                  {"approval_trace", approval_trace},
                  {"target_pid", target_pid}};
      send_json(terminal_client, ack.dump());
    }
  }

  void process_user_message(const PendingRequest &req) {
    const std::string requested_mode = "user_conversation";
    bool any_tokens_received = false;

    try {
        std::string full_response = call_llm_internal(
            "", req.message,
            "You are a helpful assistant in a Velix terminal.",
            req.user_id,
            requested_mode,
            true,
            [&](const std::string &token) {
                std::lock_guard<std::mutex> lock(terminal_client_mutex);
                if (!terminal_client.is_open()) {
                    return;
                }
                try {
                    any_tokens_received = true;
                    json resp = {{"type", "token"}, {"data", token}};
                    send_json(terminal_client, resp.dump());
                } catch (const std::exception &e) {
                    std::cerr << "[Handler] Error sending token: " << e.what()
                              << std::endl;
                }
            },
            std::nullopt);

        if (!any_tokens_received && !full_response.empty()) {
            std::lock_guard<std::mutex> lock(terminal_client_mutex);
            if (terminal_client.is_open()) {
                try {
                    json resp = {{"type", "token"}, {"data", full_response}};
                    send_json(terminal_client, resp.dump());
                } catch (const std::exception &e) {
                    std::cerr << "[Handler] Error sending full response: " << e.what()
                              << std::endl;
                }
            }
        }
    } catch (const std::exception &request_error) {
        std::cerr << "[Handler] Request error: " << request_error.what() << std::endl;
        std::lock_guard<std::mutex> lock(terminal_client_mutex);
        if (terminal_client.is_open()) {
            try {
                json resp = {{"type", "token"},
                             {"data", std::string("[Velix Error] ") + request_error.what()}};
                send_json(terminal_client, resp.dump());
            } catch (const std::exception &e) {
                std::cerr << "[Handler] Error sending error response: " << e.what()
                          << std::endl;
            }
        }
    }

    std::lock_guard<std::mutex> lock(terminal_client_mutex);
    if (terminal_client.is_open()) {
        try {
            json end_resp = {{"type", "end"}};
            send_json(terminal_client, end_resp.dump());
        } catch (const std::exception &e) {
            std::cerr << "[Handler] Error sending end response: " << e.what()
                      << std::endl;
        }
    }
  }

  SocketWrapper server;
  SocketWrapper terminal_client;
  std::mutex terminal_client_mutex;
  std::mutex approval_map_mutex;
  std::mutex request_queue_mutex;
  std::condition_variable request_queue_cv;
  std::queue<PendingRequest> request_queue;
  std::unordered_map<std::string, int> approval_trace_to_pid;
  std::atomic<bool> client_active{false};
};

int main(int argc, char **argv) {
  Handler handler;
  handler.start();
  return 0;
}
