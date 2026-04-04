#include "../runtime/sdk/cpp/velix_process.hpp"
#include <iostream>
#include <optional>

using namespace velix::core;
using namespace velix::communication;

class Handler : public VelixProcess {
public:
  Handler() : VelixProcess("terminal_handler", "handler") {}

  void run() override {
    SocketWrapper server;
    try {
      server.create_tcp_socket();
      server.bind("0.0.0.0", 6060);
      server.listen(5);
      std::cout
          << "[Handler] Streaming terminal server listening on port 6060..."
          << std::endl;

      while (is_running) {
        try {
          SocketWrapper client = server.accept();
          std::cout << "[Handler] Terminal client connected." << std::endl;

          while (is_running && client.is_open()) {
            std::string request_json = recv_json(client);
            if (request_json.empty())
              break;

            try {
              auto j = json::parse(request_json);
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
                  [&client, &any_tokens_received](const std::string &token) {
                    if (!client.is_open()) {
                      return;
                    }
                    try {
                      any_tokens_received = true;
                      json resp = {{"type", "token"}, {"data", token}};
                      send_json(client, resp.dump());
                    } catch (const std::exception &) {
                      // Client disconnect during stream: stop emitting token frames.
                    }
                  },
                  std::nullopt);

              // Robust fallback: if no tokens were received but we have a non-empty full response, send it.
              if (!any_tokens_received && !full_response.empty()) {
                  std::cout << "[Handler] Fallback: Sending full response." << std::endl;
                  json resp = {{"type", "token"}, {"data", full_response}};
                  send_json(client, resp.dump());
              }
            } catch (const std::exception &request_error) {
              std::cerr << "[Handler] Request error: " << request_error.what() << std::endl;
              json resp = {{"type", "token"},
                           {"data", std::string("[Velix Error] ") + request_error.what()}};
              send_json(client, resp.dump());
            }

            // Signal end of stream for this request.
            json end_resp = {{"type", "end"}};
            send_json(client, end_resp.dump());
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
};

int main(int argc, char **argv) {
  Handler handler;
  handler.start();
  return 0;
}
