#include "../communication/socket_wrapper.hpp"
#include "../vendor/nlohmann/json.hpp"
#include <iostream>
#include <string>

using json = nlohmann::json;
using namespace velix::communication;

// ANSI Color Codes
#define RESET   "\033[0m"
#define BOLD    "\033[1m"
#define CYAN    "\033[36m"
#define MAGENTA "\033[35m"
#define GREEN   "\033[32m"
#define GRAY    "\033[90m"

void print_header() {
    std::cout << BOLD << CYAN << "========================================" << RESET << std::endl;
    std::cout << BOLD << CYAN << "       VELIX STREAMING TERMINAL         " << RESET << std::endl;
    std::cout << BOLD << CYAN << "========================================" << RESET << std::endl;
    std::cout << GRAY << " Connected to Handler | Session: terminal_convo" << RESET << std::endl;
    std::cout << " Type 'exit' to quit.\n" << std::endl;
}

int main() {
    SocketWrapper client;
    try {
        client.create_tcp_socket();
        client.connect("127.0.0.1", 6060);
        
        print_header();

        while (true) {
            std::cout << BOLD << GREEN << "[You] " << RESET << std::flush;
            std::string input;
            if (!std::getline(std::cin, input) || input == "exit") {
                break;
            }

            if (input.empty()) continue;

            // Send user message
            json msg = {{"message", input}};
            send_json(client, msg.dump());

            std::cout << BOLD << MAGENTA << "[Velix] " << RESET << std::flush;

            // Listen for stream
            while (true) {
                std::string response = recv_json(client);
                if (response.empty()) break;

                auto j = json::parse(response);
                if (j["type"] == "token") {
                    std::cout << j["data"].get<std::string>() << std::flush;
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
