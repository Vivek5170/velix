#ifndef VELIX_PROCESS_HPP
#define VELIX_PROCESS_HPP

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "../communication/socket_wrapper.hpp"
#include "../vendor/nlohmann/json.hpp"

using json = nlohmann::json;

namespace velix {
namespace core {

enum class ProcessStatus {
    STARTING,
    RUNNING,
    WAITING_LLM,
    FINISHED,
    ERROR
};

class VelixProcess {
public:
    VelixProcess(std::string name, std::string role);
    // Global singleton pointer for OS Signal Handlers
    static VelixProcess* instance_;

    virtual ~VelixProcess();

    // The single entrypoint for the Agent Developer to override
    virtual void run() = 0;

    // --- Developer Action Hooks (Synchronous SDK Abstraction) ---
    // Natively handles the LLM -> EXEC -> LLM loop synchronously
    // by intercepting tool loops and offloading them to the Executioner
    // without waking the developer's blocked logic block.
    std::string call_llm(const std::string& convo_id, const std::string& user_message = "", const std::string& system_message = "");

    // Allows the developer to explicitly force a tool payload without LLM.
    json execute_tool(const std::string& instruction, const json& args);

    // Main event initialization. Must be called by `main()` before the process exits.
    // Connects to the Supervisor, sends REGISTER_PID, and spawns the heartbeat loop.
    void start(int override_pid = -1, const std::string& parent_tree_id = "");

    // Safe trigger to cleanly exit the `run()` loop and cleanly disconnect the OS socket.
    void shutdown();

protected:
    std::string process_name;
    std::string role; 
    std::string tree_id;
    int os_pid{-1};
    int velix_pid{-1};
    std::atomic<ProcessStatus> status{ProcessStatus::STARTING};

    // Global Supervisor Limits (Synced from REGISTER_PID response)
    int max_memory_mb{0};
    int max_runtime_sec{0};

    // Outbound persistent TCP Kernel Socket (1-1 coupling with Supervisor)
    velix::communication::SocketWrapper supervisor_socket;
    std::mutex socket_mutex; // Supervisor requests happen asynchronously to the heartbeats

    // Two-Thread Architecture tracking
    std::thread runtime_io_thread;
    std::atomic<bool> is_running{false};
    std::atomic<bool> force_terminate{false}; // Tripped by OS Signal

    // Fast-waking Kernel IO Sleep Synchronization
    std::mutex sleep_mutex;
    std::condition_variable sleep_cv;

    // Internal SDK Methods
    void run_kernel_io_loop();
    uint64_t get_current_memory_usage_mb() const;
    void send_heartbeat();
    std::string send_llm_request_stateless(const json& request_payload);
    json send_executioner_request(const std::string& instruction, const json& args);
    static int resolve_port(const std::string& service_name, int fallback);
};

} // namespace core
} // namespace velix

#endif // VELIX_PROCESS_HPP
