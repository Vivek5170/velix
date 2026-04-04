#include "../communication/socket_wrapper.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <iostream>
#include <string>
#include <csignal>

using json = nlohmann::json;
using namespace velix::communication;

// ANSI Color Codes
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define CYAN    "\033[36m"
#define MAGENTA "\033[35m"
#define GREEN   "\033[32m"
#define GRAY    "\033[90m"

std::string prompt_approval_scope() {
    while (true) {
        std::cout << BOLD << CYAN
                  << "\n[Approval] Choose scope: once | session | always | deny: "
                  << RESET << std::flush;
        std::string scope;
        if (!std::getline(std::cin, scope)) {
            return "deny";
        }
        if (scope == "once" || scope == "session" ||
            scope == "always" || scope == "deny") {
            return scope;
        }
        std::cout << "[Approval] Invalid choice. Using: deny" << std::endl;
        return "deny";
    }
}

void print_header() {
    std::cout << BOLD << CYAN << "========================================" << RESET << std::endl;
    std::cout << BOLD << CYAN << "       VELIX STREAMING TERMINAL         " << RESET << std::endl;
    std::cout << BOLD << CYAN << "========================================" << RESET << std::endl;
    std::cout << GRAY << " Connected to Handler | Session: terminal_convo" << RESET << std::endl;
    std::cout << " Type 'exit' to quit.\n" << std::endl;
}

volatile std::sig_atomic_t is_running = 1;

void signal_handler(int signal) {
    if (signal == SIGINT) {
        is_running = 0;
        std::cout << "\n[Terminal] SIGINT received. Shutting down gracefully..." << std::endl;
    }
}

int main() {
    std::signal(SIGINT, signal_handler);

    SocketWrapper client;
    try {
        client.create_tcp_socket();
        client.connect("127.0.0.1", 6060);

        print_header();

        while (is_running) {
            std::cout << BOLD << GREEN << "[You] " << RESET << std::flush;
            std::string input;
            if (!std::getline(std::cin, input) || input == "exit") {
                break;
            }

            if (input.empty()) continue;

            // Ensure user_conversation mode with empty conversation_id
            json msg = {
                {"message", input},
                {"conversation_id", ""},
                {"user_id", "terminal_cli_user_1"}
            };
            send_json(client, msg.dump());

            std::cout << BOLD << MAGENTA << "[Velix] " << RESET << std::flush;

            while (true) {
                std::string response = recv_json(client);
                if (response.empty()) break;

                auto j = json::parse(response);
                if (j["type"] == "token") {
                    std::cout << j["data"].get<std::string>() << std::flush;
                } else if (j["type"] == "tool_start") {
                    std::cout << "\n[Tool Start] " << j["tool"].get<std::string>() << " "
                              << j["args"].dump() << "\n" << std::flush;
                } else if (j["type"] == "tool_finish") {
                    std::cout << "\n[Tool Finish] " << j["tool"].get<std::string>() << " "
                              << j["result"].dump() << "\n" << std::flush;
                } else if (j["type"] == "notify") {
                    const std::string purpose = j.value("purpose", "");
                    const json payload = j.value("payload", json::object());
                    std::cout << "\n[Handler Notify] purpose="
                              << purpose << " user_id="
                              << j.value("user_id", "") << " payload="
                              << payload.dump() << "\n" << std::flush;

                    if (purpose == "APPROVAL_REQUEST") {
                        const std::string approval_trace =
                            payload.value("approval_trace", std::string(""));
                        const std::string command =
                            payload.value("command", std::string(""));
                        const std::string description =
                            payload.value("description", std::string(""));

                        std::cout << "[Approval] Command: " << command << "\n"
                                  << "[Approval] Reason : " << description << std::endl;

                        const std::string scope = prompt_approval_scope();
                        json approval = {
                            {"type", "approval_reply"},
                            {"approval_trace", approval_trace},
                            {"scope", scope}
                        };
                        send_json(client, approval.dump());
                    }
                } else if (j["type"] == "approval_ack") {
                    std::cout << "[Approval] Reply delivered for trace="
                              << j.value("approval_trace", "")
                              << " target_pid=" << j.value("target_pid", -1)
                              << "\n" << std::flush;
                } else if (j["type"] == "end") {
                    std::cout << "\n" << std::endl;
                    break;
                }
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "\n" << BOLD << "\033[31m[!] Terminal Error: " << RESET << e.what() << std::endl;
        return 1;
    }

    std::cout << GRAY << "Shutting down..." << RESET << std::endl;
    return 0;
}
