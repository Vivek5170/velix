#ifndef VELIX_PROCESS_HPP
#define VELIX_PROCESS_HPP

#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>

#include "../../../communication/socket_wrapper.hpp"
#include "../../../vendor/nlohmann/json.hpp"

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

    // --- Creator-Facing SDK Hooks (Synchronous Abstraction) ---
    // These are the primary APIs a process creator should use in run().
    // They may throw std::runtime_error on transport/protocol failures.
    // Catch exceptions in your run() only if you can recover locally.
    // Otherwise let them propagate so the Supervisor receives accurate state.

    // Natively handles the LLM -> EXEC -> LLM loop synchronously
    // by intercepting tool loops and offloading them to the Executioner
    // without waking the developer's blocked logic block.
    // Throws on scheduler rejection, timeout, malformed responses, or tool
    // crash propagation.
    std::string call_llm(const std::string& convo_id, const std::string& user_message = "", const std::string& system_message = "");

    // Allows the developer to explicitly force a tool payload without LLM.
    // Throws on executioner connectivity failure, spawn failure, timeout,
    // or child termination.
    json execute_tool(const std::string& instruction, const json& args);

    // Main lifecycle initialization. Must be called by main() before process exit.
    // Connects to the Supervisor, sends REGISTER_PID, and spawns the heartbeat loop.
    // Throws if registration/connectivity contracts fail.
    void start(int override_pid = -1, const std::string& parent_tree_id = "");


    // Safe trigger to cleanly exit the `run()` loop and cleanly disconnect the OS socket.
    void shutdown();

protected:
    std::string process_name;
    std::string role; 
    std::string tree_id;
    json params; // Injected by Executioner on startup
    int os_pid{-1};
    int velix_pid{-1};
    int parent_pid{-1};
    bool is_root{false};
    bool result_reported{false}; // Tracks if a final tool result was dispatched
    std::string entry_trace_id;  // The trace ID that launched this process
    std::atomic<ProcessStatus> status{ProcessStatus::STARTING};

    // Global Supervisor Limits (Synced from REGISTER_PID response)
    int max_memory_mb{0};
    int max_runtime_sec{0};

    // Outbound persistent TCP Kernel Socket (1-1 coupling with Supervisor)
    velix::communication::SocketWrapper supervisor_socket;
    std::mutex socket_mutex; // Supervisor requests happen asynchronously to the heartbeats

    // Outbound persistent TCP Bus Socket (for peer-to-peer IPC and Results)
    velix::communication::SocketWrapper bus_socket;
    std::mutex bus_mutex;
    std::thread bus_listener_thread;

    // Async Result Waiting
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::map<std::string, json> response_map; // trace_id -> payload

    // Two-Thread Architecture tracking
    std::thread runtime_io_thread;
    std::atomic<bool> is_running{false};
    std::atomic<bool> force_terminate{false}; // Tripped by OS Signal

    // Fast-waking Kernel IO Sleep Synchronization
    std::mutex sleep_mutex;
    std::condition_variable sleep_cv;

    // RAII Termination Reporter
    struct ResultGuard {
        VelixProcess* proc;
        ResultGuard(VelixProcess* p) : proc(p) {}
        ~ResultGuard() {
            if (proc && !proc->result_reported && proc->parent_pid > 0) {
                json completion = {
                    {"status", "completed"},
                    {"exit_reason", "normal"},
                    {"pid", proc->velix_pid}
                };
                proc->report_result(proc->parent_pid, completion, proc->entry_trace_id);
            }
        }
    };

    // Internal SDK Methods
    void run_kernel_io_loop();
    void bus_listener_loop(); // Handles IPM_PUSH messages from the router
    uint64_t get_current_memory_usage_mb() const;
    void send_heartbeat();
    std::string send_llm_request_stateless(const json& request_payload);
    json exec_velix_process(const std::string& name, const json& params, const std::string& trace_id = "");
    void report_result(int target_pid, const json& data, const std::string& trace_id = "");
    static int resolve_port(const std::string& service_name, int fallback);
};

} // namespace core
} // namespace velix

#endif // VELIX_PROCESS_HPP
