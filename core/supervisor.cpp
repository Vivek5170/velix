#include "supervisor.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/logger.hpp"
#include "../vendor/nlohmann/json.hpp"

#include <atomic>
#include <chrono>
#include <fstream>
#include <mutex>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>
#include <cerrno>
#endif

using json = nlohmann::json;

namespace velix::core {

namespace {

constexpr int kDefaultSupervisorPort = 5173;

struct SupervisorConfig {
	int heartbeat_timeout_sec = 15;
	int watchdog_interval_ms = 1000;
	bool require_auth_token = false;
	std::string auth_token = "";
	bool exempt_system_tree_limits = true;

	int max_processes_per_tree = 64;
	int max_tree_runtime_sec = 3600;
	double max_memory_per_tree_mb = 2048.0;
	int max_llm_requests_per_tree = 1000;
};

SupervisorConfig load_supervisor_config() {
	SupervisorConfig config;

	std::ifstream file("config/supervisor.json");
	if (!file.is_open()) {
		LOG_WARN_CTX("config/supervisor.json not found, using defaults",
			"supervisor", "", -1, "config_default");
		return config;
	}

	try {
		json cfg;
		file >> cfg;

		config.heartbeat_timeout_sec = cfg.value("heartbeat_timeout_sec", config.heartbeat_timeout_sec);
		config.watchdog_interval_ms = cfg.value("watchdog_interval_ms", config.watchdog_interval_ms);
		config.require_auth_token = cfg.value("require_auth_token", config.require_auth_token);
		config.auth_token = cfg.value("auth_token", config.auth_token);
		config.exempt_system_tree_limits = cfg.value("exempt_system_tree_limits", config.exempt_system_tree_limits);

		const json limits = cfg.value("limits", json::object());
		config.max_processes_per_tree = limits.value("max_processes_per_tree", config.max_processes_per_tree);
		config.max_tree_runtime_sec = limits.value("max_tree_runtime_sec", config.max_tree_runtime_sec);
		config.max_memory_per_tree_mb = limits.value("max_memory_per_tree_mb", config.max_memory_per_tree_mb);
		config.max_llm_requests_per_tree = limits.value("max_llm_requests_per_tree", config.max_llm_requests_per_tree);

		LOG_INFO_CTX("Loaded supervisor config", "supervisor", "", -1, "config_loaded");
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("Failed to parse config/supervisor.json: ") + e.what(),
			"supervisor", "", -1, "config_parse_error");
	}

	return config;
}

enum class ProcessStatus {
	STARTING,
	RUNNING,
	WAITING_LLM,
	WAITING_EXEC,
	FINISHED,
	ERROR,
	KILLED
};

enum class TreeStatus {
	ACTIVE,
	WAITING_LLM_SLOT,
	WAITING_EXECUTION,
	COMPLETED,
	FAILED,
	KILLED
};

std::string to_string(ProcessStatus status) {
	switch (status) {
		case ProcessStatus::STARTING: return "STARTING";
		case ProcessStatus::RUNNING: return "RUNNING";
		case ProcessStatus::WAITING_LLM: return "WAITING_LLM";
		case ProcessStatus::WAITING_EXEC: return "WAITING_EXEC";
		case ProcessStatus::FINISHED: return "FINISHED";
		case ProcessStatus::ERROR: return "ERROR";
		case ProcessStatus::KILLED: return "KILLED";
	}
	return "ERROR";
}

std::string to_string(TreeStatus status) {
	switch (status) {
		case TreeStatus::ACTIVE: return "ACTIVE";
		case TreeStatus::WAITING_LLM_SLOT: return "WAITING_LLM_SLOT";
		case TreeStatus::WAITING_EXECUTION: return "WAITING_EXECUTION";
		case TreeStatus::COMPLETED: return "COMPLETED";
		case TreeStatus::FAILED: return "FAILED";
		case TreeStatus::KILLED: return "KILLED";
	}
	return "FAILED";
}

ProcessStatus parse_process_status(const std::string& status) {
	if (status == "STARTING") return ProcessStatus::STARTING;
	if (status == "RUNNING") return ProcessStatus::RUNNING;
	if (status == "WAITING_LLM") return ProcessStatus::WAITING_LLM;
	if (status == "WAITING_EXEC") return ProcessStatus::WAITING_EXEC;
	if (status == "FINISHED") return ProcessStatus::FINISHED;
	if (status == "KILLED") return ProcessStatus::KILLED;
	return ProcessStatus::ERROR;
}

TreeStatus parse_tree_status(const std::string& status) {
	if (status == "ACTIVE") return TreeStatus::ACTIVE;
	if (status == "WAITING_LLM_SLOT") return TreeStatus::WAITING_LLM_SLOT;
	if (status == "WAITING_EXECUTION") return TreeStatus::WAITING_EXECUTION;
	if (status == "COMPLETED") return TreeStatus::COMPLETED;
	if (status == "KILLED") return TreeStatus::KILLED;
	return TreeStatus::FAILED;
}

struct ProcessInfo {
	int pid = -1;
	std::string tree_id;
	int parent_pid = -1;
	std::string role = "unknown";
	std::string llm_key = "";
	ProcessStatus status = ProcessStatus::STARTING;
	double memory_mb = 0.0;
	std::chrono::steady_clock::time_point last_heartbeat = std::chrono::steady_clock::now();
};

struct TreeInfo {
	std::string tree_id;
	TreeStatus status = TreeStatus::ACTIVE;
	int root_pid = -1;
	std::string origin = "unknown";
	int llm_request_count = 0;
	std::chrono::steady_clock::time_point created_at = std::chrono::steady_clock::now();
};

bool is_terminal_status(ProcessStatus status) {
	return status == ProcessStatus::FINISHED ||
		status == ProcessStatus::KILLED ||
		status == ProcessStatus::ERROR;
}

bool is_system_handler_tree(const std::string& tree_id) {
	return tree_id == "TREE_HANDLER";
}

bool is_limit_enabled(int value) {
	return value > 0;
}

bool is_limit_enabled(double value) {
	return value > 0.0;
}

bool terminate_os_process(int pid) {
	if (pid <= 0) {
		return false;
	}

#ifdef _WIN32
	HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE, static_cast<DWORD>(pid));
	if (process == nullptr) {
		return false;
	}
	const BOOL terminated = TerminateProcess(process, 1);
	CloseHandle(process);
	return terminated == TRUE;
#else
	if (::kill(pid, SIGTERM) == -1) {
		if (errno == ESRCH) {
			return true;
		}
	}

	std::this_thread::sleep_for(std::chrono::milliseconds(150));

	if (::kill(pid, 0) == 0) {
		if (::kill(pid, SIGKILL) == -1) {
			return errno == ESRCH;
		}
	}

	return true;
#endif
}

class SupervisorService {
public:
	SupervisorService()
		: config_(load_supervisor_config()), running_(false), tree_counter_(1) {
		register_system_handler_tree();
	}

	~SupervisorService() {
		stop();
	}

	void start(int port) {
		if (running_) {
			return;
		}

		running_ = true;
		LOG_INFO_CTX("Supervisor service starting", "supervisor", "", -1, "startup");

		watchdog_thread_ = std::thread([this]() { watchdog_loop(); });

		try {
			{
				std::lock_guard<std::mutex> lock(server_mutex_);
				server_socket_ = std::make_unique<velix::communication::SocketWrapper>();
				server_socket_->create_tcp_socket();
				server_socket_->bind("127.0.0.1", static_cast<uint16_t>(port));
				server_socket_->listen(8);
			}

			LOG_INFO_CTX("Supervisor listening on 127.0.0.1:" + std::to_string(port),
						 "supervisor", "", -1, "listen");

			while (running_) {
				try {
					velix::communication::SocketWrapper client;
					{
						std::lock_guard<std::mutex> lock(server_mutex_);
						if (!server_socket_ || !server_socket_->is_open()) {
							break;
						}
						client = server_socket_->accept();
					}
					std::thread(&SupervisorService::handle_client, this, std::move(client)).detach();
				} catch (const std::exception& e) {
					if (!running_) {
						break;
					}
					LOG_WARN_CTX(std::string("Supervisor accept error: ") + e.what(),
								 "supervisor", "", -1, "accept_error");
				}
			}

			stop();
		} catch (const std::exception& e) {
			running_ = false;
			join_watchdog();
			LOG_ERROR_CTX(std::string("Supervisor startup failed: ") + e.what(),
						  "supervisor", "", -1, "startup_error");
			throw;
		}
	}

	void stop() {
		const bool was_running = running_.exchange(false);
		if (!was_running) {
			join_watchdog();
			return;
		}

		{
			std::lock_guard<std::mutex> lock(server_mutex_);
			if (server_socket_ && server_socket_->is_open()) {
				server_socket_->close();
			}
			server_socket_.reset();
		}

		join_watchdog();
		LOG_INFO_CTX("Supervisor stopped gracefully", "supervisor", "", -1, "shutdown");
	}

private:
	SupervisorConfig config_;

	std::mutex state_mutex_;
	std::unordered_map<int, ProcessInfo> process_table_;
	std::unordered_map<std::string, TreeInfo> tree_table_;
	std::mutex server_mutex_;
	std::unique_ptr<velix::communication::SocketWrapper> server_socket_;

	std::atomic<bool> running_;
	std::atomic<std::uint64_t> tree_counter_;
	std::thread watchdog_thread_;

	void join_watchdog() {
		if (watchdog_thread_.joinable()) {
			watchdog_thread_.join();
		}
	}

	bool validate_auth(const json& message, std::string& error) const {
		if (!config_.require_auth_token) {
			return true;
		}

		const json metadata = message.value("metadata", json::object());
		const std::string auth_token = metadata.value("auth_token", "");
		if (auth_token.empty() || auth_token != config_.auth_token) {
			error = "auth failed";
			return false;
		}

		return true;
	}

	bool validate_message_shape(const json& message, std::string& error) const {
		if (!message.is_object()) {
			error = "message must be a JSON object";
			return false;
		}

		if (!message.contains("message_type") || !message["message_type"].is_string()) {
			error = "missing or invalid message_type";
			return false;
		}

		static const std::unordered_set<std::string> allowed_types = {
			"REGISTER_PID", "HEARTBEAT", "LLM_REQUEST", "TREE_STATUS", "TREE_KILL"
		};

		const std::string message_type = message["message_type"].get<std::string>();
		if (allowed_types.find(message_type) == allowed_types.end()) {
			error = "unsupported message_type: " + message_type;
			return false;
		}

		if (message_type == "REGISTER_PID" || message_type == "HEARTBEAT") {
			if (!message.contains("pid") || !message["pid"].is_number_integer() || message["pid"].get<int>() <= 0) {
				error = message_type + " requires positive integer pid";
				return false;
			}
		}

		return true;
	}

	void mark_tree_failed_unlocked(const std::string& tree_id) {
		auto tree_it = tree_table_.find(tree_id);
		if (tree_it != tree_table_.end() &&
			tree_it->second.status != TreeStatus::KILLED &&
			tree_it->second.status != TreeStatus::COMPLETED) {
			tree_it->second.status = TreeStatus::FAILED;
		}
	}

	bool mark_tree_completed_if_done_unlocked(const std::string& tree_id) {
		if (is_system_handler_tree(tree_id)) {
			return false;
		}

		auto tree_it = tree_table_.find(tree_id);
		if (tree_it == tree_table_.end()) {
			return false;
		}

		if (tree_it->second.status == TreeStatus::KILLED ||
			tree_it->second.status == TreeStatus::FAILED ||
			tree_it->second.status == TreeStatus::COMPLETED) {
			return false;
		}

		bool has_processes = false;
		for (const auto& [pid, process] : process_table_) {
			if (process.tree_id != tree_id) {
				continue;
			}

			has_processes = true;
			if (!is_terminal_status(process.status)) {
				return false;
			}
		}

		if (has_processes) {
			tree_it->second.status = TreeStatus::COMPLETED;
			return true;
		}

		return false;
	}

	std::vector<int> collect_active_tree_pids_unlocked(const std::string& tree_id) {
		std::vector<int> pids;
		for (const auto& [pid, process] : process_table_) {
			if (process.tree_id == tree_id && !is_terminal_status(process.status)) {
				pids.push_back(pid);
			}
		}
		return pids;
	}

	double compute_tree_memory_mb_unlocked(const std::string& tree_id) {
		double total = 0.0;
		for (const auto& [pid, process] : process_table_) {
			if (process.tree_id == tree_id && !is_terminal_status(process.status)) {
				total += process.memory_mb;
			}
		}
		return total;
	}

	void terminate_processes(const std::vector<int>& pids, const std::string& tree_id, const std::string& event) {
		for (int pid : pids) {
			const bool ok = terminate_os_process(pid);
			if (!ok) {
				LOG_WARN_CTX("Failed to terminate pid " + std::to_string(pid),
					"supervisor", tree_id, pid, event + "_kill_failed");
			}
		}
	}

	void register_system_handler_tree() {
		std::lock_guard<std::mutex> lock(state_mutex_);
		TreeInfo handler_tree;
		handler_tree.tree_id = "TREE_HANDLER";
		handler_tree.status = TreeStatus::ACTIVE;
		handler_tree.origin = "system";
		tree_table_[handler_tree.tree_id] = handler_tree;
	}

	std::string create_tree_unlocked(const std::string& origin) {
		std::ostringstream oss;
		oss << "TREE_" << tree_counter_++;

		TreeInfo tree_info;
		tree_info.tree_id = oss.str();
		tree_info.status = TreeStatus::ACTIVE;
		tree_info.origin = origin;
		tree_info.created_at = std::chrono::steady_clock::now();

		tree_table_[tree_info.tree_id] = tree_info;
		return tree_info.tree_id;
	}

	json build_process_json(const ProcessInfo& process) const {
		return {
			{"pid", process.pid},
			{"tree_id", process.tree_id},
			{"parent_pid", process.parent_pid},
			{"role", process.role},
			{"llm_key", process.llm_key},
			{"status", to_string(process.status)}
		};
	}

	json build_tree_json(const TreeInfo& tree) const {
		return {
			{"tree_id", tree.tree_id},
			{"status", to_string(tree.status)},
			{"root_pid", tree.root_pid},
			{"origin", tree.origin}
		};
	}

	void handle_client(velix::communication::SocketWrapper client_sock) {
		try {
			const std::string request_raw = velix::communication::recv_json(client_sock);
			json request = json::parse(request_raw);
			json response = handle_message(request);
			velix::communication::send_json(client_sock, response.dump());
		} catch (const std::exception& e) {
			LOG_ERROR_CTX(std::string("Supervisor request error: ") + e.what(),
						  "supervisor", "", -1, "request_error");
			try {
				json error = {
					{"status", "error"},
					{"error", e.what()}
				};
				velix::communication::send_json(client_sock, error.dump());
			} catch (...) {
				LOG_ERROR_CTX("Failed to send supervisor error response",
							  "supervisor", "", -1, "response_error");
			}
		}
	}

	json handle_message(const json& message) {
		std::string validation_error;
		if (!validate_message_shape(message, validation_error)) {
			return {
				{"status", "error"},
				{"error", validation_error}
			};
		}

		if (!validate_auth(message, validation_error)) {
			return {
				{"status", "error"},
				{"error", validation_error}
			};
		}

		const std::string message_type = message.value("message_type", "");

		if (message_type == "REGISTER_PID") {
			return handle_register_pid(message);
		}
		if (message_type == "HEARTBEAT") {
			return handle_heartbeat(message);
		}
		if (message_type == "LLM_REQUEST") {
			return handle_llm_request(message);
		}
		if (message_type == "TREE_STATUS") {
			return handle_tree_status(message);
		}
		if (message_type == "TREE_KILL") {
			return handle_tree_kill(message);
		}

		return {
			{"status", "error"},
			{"error", "unsupported message_type: " + message_type}
		};
	}

	json handle_register_pid(const json& message) {
		const int pid = message.value("pid", -1);
		if (pid <= 0) {
			return {
				{"status", "error"},
				{"error", "REGISTER_PID requires valid pid"}
			};
		}

		const std::string requested_tree = message.value("tree_id", "");
		const int parent_pid = message.value("parent_pid", -1);
		const json payload = message.value("payload", json::object());
		const std::string role = payload.value("role", "unknown");
		const std::string llm_key = payload.value("llm_key", "");
		const ProcessStatus status = parse_process_status(payload.value("status", "STARTING"));
		const double memory_mb = payload.value("memory_mb", 0.0);

		std::lock_guard<std::mutex> lock(state_mutex_);

		std::string tree_id = requested_tree;
		if (tree_id.empty()) {
			tree_id = create_tree_unlocked("handler");
		} else if (tree_table_.find(tree_id) == tree_table_.end()) {
			TreeInfo external_tree;
			external_tree.tree_id = tree_id;
			external_tree.status = TreeStatus::ACTIVE;
			external_tree.origin = "external";
			tree_table_[tree_id] = external_tree;
		}

		int active_count = 0;
		for (const auto& [existing_pid, existing] : process_table_) {
			if (existing.tree_id == tree_id && !is_terminal_status(existing.status)) {
				++active_count;
			}
		}

		if (is_limit_enabled(config_.max_processes_per_tree) &&
			active_count >= config_.max_processes_per_tree) {
			if (config_.exempt_system_tree_limits && is_system_handler_tree(tree_id)) {
				// system tree intentionally exempt from per-tree limits
			} else {
			return {
				{"status", "error"},
				{"error", "max_processes_per_tree exceeded"},
				{"tree_id", tree_id}
			};
			}
		}

		auto tree_it = tree_table_.find(tree_id);
		if (tree_it != tree_table_.end() && is_limit_enabled(config_.max_tree_runtime_sec)) {
			const auto age = std::chrono::duration_cast<std::chrono::seconds>(
				std::chrono::steady_clock::now() - tree_it->second.created_at).count();
			if (age > config_.max_tree_runtime_sec &&
				!(config_.exempt_system_tree_limits && is_system_handler_tree(tree_id))) {
				return {
					{"status", "error"},
					{"error", "max_tree_runtime_sec exceeded"},
					{"tree_id", tree_id}
				};
			}
		}

		ProcessInfo process;
		process.pid = pid;
		process.tree_id = tree_id;
		process.parent_pid = parent_pid;
		process.role = role;
		process.llm_key = llm_key;
		process.status = status;
		process.memory_mb = memory_mb;
		process.last_heartbeat = std::chrono::steady_clock::now();

		process_table_[pid] = process;

		TreeInfo& tree = tree_table_[tree_id];
		if (parent_pid <= 0 && tree.root_pid <= 0) {
			tree.root_pid = pid;
		}

		LOG_INFO_CTX("Registered pid " + std::to_string(pid) + " in " + tree_id,
					 "supervisor", tree_id, pid, "register_pid");

		return {
			{"status", "ok"},
			{"tree_id", tree_id},
			{"process", build_process_json(process)}
		};
	}

	json handle_heartbeat(const json& message) {
		const int pid = message.value("pid", -1);
		if (pid <= 0) {
			return {
				{"status", "error"},
				{"error", "HEARTBEAT requires valid pid"}
			};
		}

		const json payload = message.value("payload", json::object());
		const std::string status_text = payload.value("status", "");
		const double memory_mb = payload.value("memory_mb", -1.0);
		std::vector<int> kill_pids;
		std::string tree_id_for_kill;
		bool tree_completed = false;

		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			auto it = process_table_.find(pid);
			if (it == process_table_.end()) {
				return {
					{"status", "error"},
					{"error", "pid not registered"}
				};
			}

			it->second.last_heartbeat = std::chrono::steady_clock::now();
			if (!status_text.empty()) {
				it->second.status = parse_process_status(status_text);
			}
			if (memory_mb >= 0.0) {
				it->second.memory_mb = memory_mb;
			}

			tree_id_for_kill = it->second.tree_id;
			if (is_limit_enabled(config_.max_memory_per_tree_mb)) {
				if (!(config_.exempt_system_tree_limits && is_system_handler_tree(tree_id_for_kill))) {
					const double tree_memory = compute_tree_memory_mb_unlocked(tree_id_for_kill);
					if (tree_memory > config_.max_memory_per_tree_mb) {
						mark_tree_failed_unlocked(tree_id_for_kill);
						kill_pids = collect_active_tree_pids_unlocked(tree_id_for_kill);

						for (int kill_pid : kill_pids) {
							auto proc_it = process_table_.find(kill_pid);
							if (proc_it != process_table_.end()) {
								proc_it->second.status = ProcessStatus::KILLED;
							}
						}
					}
				}
			}

			tree_completed = mark_tree_completed_if_done_unlocked(tree_id_for_kill);
		}

		json response = {
			{"status", "ok"},
			{"pid", pid}
		};

		if (!kill_pids.empty()) {
			LOG_ERROR_CTX("Tree memory limit exceeded; terminating tree",
				"supervisor", tree_id_for_kill, pid, "memory_limit_exceeded");

			response["warning"] = "memory limit exceeded; tree processes terminated";
			terminate_processes(kill_pids, tree_id_for_kill, "memory_limit");
		}

		if (tree_completed) {
			response["tree_status"] = "COMPLETED";
			LOG_INFO_CTX("Tree completed", "supervisor", tree_id_for_kill, pid, "tree_completed");
		}

		return response;
	}

	json handle_llm_request(const json& message) {
		const std::string tree_id = message.value("tree_id", "");
		if (tree_id.empty()) {
			return {
				{"status", "error"},
				{"error", "LLM_REQUEST requires tree_id"}
			};
		}

		std::lock_guard<std::mutex> lock(state_mutex_);
		auto tree_it = tree_table_.find(tree_id);
		if (tree_it == tree_table_.end()) {
			return {
				{"status", "error"},
				{"error", "tree not found"}
			};
		}

		tree_it->second.llm_request_count += 1;
		if (is_limit_enabled(config_.max_llm_requests_per_tree) &&
			tree_it->second.llm_request_count > config_.max_llm_requests_per_tree &&
			!(config_.exempt_system_tree_limits && is_system_handler_tree(tree_id))) {
			tree_it->second.status = TreeStatus::FAILED;
			return {
				{"status", "error"},
				{"error", "max_llm_requests_per_tree exceeded"},
				{"tree_id", tree_id}
			};
		}

		return {
			{"status", "ok"},
			{"tree_id", tree_id},
			{"llm_request_count", tree_it->second.llm_request_count}
		};
	}

	json handle_tree_status(const json& message) {
		const json payload = message.value("payload", json::object());
		const std::string tree_id = payload.value("tree_id", message.value("tree_id", ""));

		std::lock_guard<std::mutex> lock(state_mutex_);
		if (tree_id.empty()) {
			json trees = json::array();
			for (const auto& [id, tree] : tree_table_) {
				trees.push_back(build_tree_json(tree));
			}

			return {
				{"status", "ok"},
				{"trees", trees}
			};
		}

		auto tree_it = tree_table_.find(tree_id);
		if (tree_it == tree_table_.end()) {
			return {
				{"status", "error"},
				{"error", "tree not found"}
			};
		}

		const std::string requested_status = payload.value("set_status", "");
		if (!requested_status.empty()) {
			tree_it->second.status = parse_tree_status(requested_status);
		}

		json processes = json::array();
		for (const auto& [pid, proc] : process_table_) {
			if (proc.tree_id == tree_id) {
				processes.push_back(build_process_json(proc));
			}
		}

		return {
			{"status", "ok"},
			{"tree", build_tree_json(tree_it->second)},
			{"processes", processes}
		};
	}

	json handle_tree_kill(const json& message) {
		const json payload = message.value("payload", json::object());
		const std::string tree_id = payload.value("tree_id", message.value("tree_id", ""));
		if (tree_id.empty()) {
			return {
				{"status", "error"},
				{"error", "TREE_KILL requires tree_id"}
			};
		}

		std::vector<int> kill_pids;
		{
			std::lock_guard<std::mutex> lock(state_mutex_);
			auto tree_it = tree_table_.find(tree_id);
			if (tree_it == tree_table_.end()) {
				return {
					{"status", "error"},
					{"error", "tree not found"}
				};
			}

			tree_it->second.status = TreeStatus::KILLED;
			for (auto& [pid, process] : process_table_) {
				if (process.tree_id == tree_id && process.status != ProcessStatus::FINISHED) {
					process.status = ProcessStatus::KILLED;
					kill_pids.push_back(pid);
				}
			}
		}

		terminate_processes(kill_pids, tree_id, "tree_kill");

		LOG_WARN_CTX("Tree killed: " + tree_id,
					 "supervisor", tree_id, -1, "tree_kill");

		return {
			{"status", "ok"},
			{"tree_id", tree_id},
			{"tree_status", "KILLED"}
		};
	}

	void watchdog_loop() {
		while (running_) {
			std::this_thread::sleep_for(std::chrono::milliseconds(config_.watchdog_interval_ms));
			const auto now = std::chrono::steady_clock::now();
			std::unordered_map<int, std::string> pids_to_kill;
			std::unordered_set<std::string> trees_timed_out;
			std::vector<std::string> trees_completed;

			{
				std::lock_guard<std::mutex> lock(state_mutex_);

				for (auto& [pid, process] : process_table_) {
					const bool completed = is_terminal_status(process.status);

					if (completed) {
						continue;
					}

					if (now - process.last_heartbeat > std::chrono::seconds(config_.heartbeat_timeout_sec)) {
						process.status = ProcessStatus::ERROR;
						pids_to_kill[pid] = process.tree_id;
						trees_timed_out.insert(process.tree_id);
					}
				}

				if (is_limit_enabled(config_.max_tree_runtime_sec)) {
					for (auto& [tree_id, tree] : tree_table_) {
						if (tree.status == TreeStatus::COMPLETED || tree.status == TreeStatus::KILLED) {
							continue;
						}

						if (config_.exempt_system_tree_limits && is_system_handler_tree(tree_id)) {
							continue;
						}

						const auto tree_age_sec = std::chrono::duration_cast<std::chrono::seconds>(
							now - tree.created_at).count();
						if (tree_age_sec > config_.max_tree_runtime_sec) {
							tree.status = TreeStatus::FAILED;
							const auto tree_pids = collect_active_tree_pids_unlocked(tree_id);
							for (int pid : tree_pids) {
								pids_to_kill[pid] = tree_id;
							}
						}
					}
				}

				for (const auto& tree_id : trees_timed_out) {
					mark_tree_failed_unlocked(tree_id);
				}

				for (const auto& [tree_id, tree] : tree_table_) {
					if (mark_tree_completed_if_done_unlocked(tree_id)) {
						trees_completed.push_back(tree_id);
					}
				}
			}

			for (const auto& [pid, tree_id] : pids_to_kill) {
				terminate_os_process(pid);
				LOG_ERROR_CTX("Terminated pid " + std::to_string(pid) + " due watchdog policy",
					"supervisor", tree_id, pid, "watchdog_termination");
			}

			for (const auto& tree_id : trees_completed) {
				LOG_INFO_CTX("Tree completed", "supervisor", tree_id, -1, "tree_completed");
			}
		}
	}
};

SupervisorService& supervisor_service() {
	static SupervisorService service;
	return service;
}

} // namespace

void start_supervisor(int port) {
	supervisor_service().start(port);
}

void stop_supervisor() {
	supervisor_service().stop();
}

} // namespace velix::core
