#include "../runtime/sdk/cpp/velix_process.hpp"
#include <iostream>
#include <optional>
#include <mutex>
#include <unordered_map>

using namespace velix::core;
using namespace velix::communication;

class Handler : public VelixProcess {
public:
  Handler() : VelixProcess("terminal_handler", "handler") {}

  void on_shutdown() override {
    std::lock_guard<std::mutex> lock(terminal_client_mutex);
    if (terminal_client.is_open()) {
      terminal_client.close();
    }
    if (server.is_open()) {
      server.close();
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
        if (!approval_trace.empty() && source_pid > 0) {
          std::lock_guard<std::mutex> lock(approval_map_mutex);
          approval_trace_to_pid[approval_trace] = source_pid;
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
          }
          std::cout << "[Handler] Terminal client connected." << std::endl;

          while (is_running && terminal_client.is_open()) {
            std::string request_json = recv_json(terminal_client);
            if (request_json.empty())
              break;

            try {
              auto j = json::parse(request_json);

              if (j.value("type", std::string("")) == "approval_reply") {
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
                  send_message(target_pid, "APPROVAL_REPLY",
                               {{"approval_trace", approval_trace},
                                {"scope", scope}});
                }

                json ack = {{"type", "approval_ack"},
                            {"approval_trace", approval_trace},
                            {"target_pid", target_pid}};
                send_json(terminal_client, ack.dump());
                continue;
              }

              if (!j.contains("message"))
                continue;

              std::string user_msg = j["message"];
              const std::string terminal_user_id =
                  (j.contains("user_id") && j["user_id"].is_string() &&
                   !j["user_id"].get<std::string>().empty())
                      ? j["user_id"].get<std::string>()
                      : "terminal_cli_user";
                // Terminal handler always uses per-user conversation semantics.
                const std::string requested_mode = "user_conversation";

              std::cout << "[Handler] Received: " << user_msg << std::endl;

              // Call the LLM with streaming enabled
              bool any_tokens_received = false;
              std::string full_response = call_llm_internal(
                  "", user_msg,
                  "You are a helpful assistant in a Velix terminal.",
                  terminal_user_id,
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
                    } catch (const std::exception &) {
                      // Client disconnect during stream: stop emitting token frames.
                    }
                  },
                  std::nullopt);

              // Robust fallback: if no tokens were received but we have a non-empty full response, send it.
              if (!any_tokens_received && !full_response.empty()) {
                  std::cout << "[Handler] Fallback: Sending full response." << std::endl;
                  json resp = {{"type", "token"}, {"data", full_response}};
                  send_json(terminal_client, resp.dump());
              }
            } catch (const std::exception &request_error) {
              std::cerr << "[Handler] Request error: " << request_error.what() << std::endl;
              json resp = {{"type", "token"},
                           {"data", std::string("[Velix Error] ") + request_error.what()}};
              send_json(terminal_client, resp.dump());
            }

            // Signal end of stream for this request.
            json end_resp = {{"type", "end"}};
            send_json(terminal_client, end_resp.dump());
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
  SocketWrapper server;
  SocketWrapper terminal_client;
  std::mutex terminal_client_mutex;
  std::mutex approval_map_mutex;
  std::unordered_map<std::string, int> approval_trace_to_pid;
};

int main(int argc, char **argv) {
  Handler handler;
  handler.start();
  return 0;
}
