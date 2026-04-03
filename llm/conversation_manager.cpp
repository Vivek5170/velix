#include "conversation_manager.hpp"
#include "compacter.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>

namespace fs = std::filesystem;

namespace velix::llm {

namespace {

// ── Helpers ────────────────────────────────────────────────────────────────

std::string random_hex_id(const std::string& prefix) {
	static thread_local std::mt19937_64 rng{std::random_device{}()};
	const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto r = rng();
	std::ostringstream oss;
	oss << prefix << '-' << std::hex << now << '-' << (r & 0xFFFFFFFFULL);
	return oss.str();
}

std::string generate_request_id() {
	return random_hex_id("REQ");
}

bool is_valid_role(const std::string& role) {
	return role == "system" || role == "user" || role == "assistant" || role == "tool" || role == "agent";
}

std::string canonicalize_role(std::string role) {
	if (role == "agent") {
		return "assistant";
	}
	return role;
}

// Keep only alphanumeric chars for safe filename embedding
std::string sanitize_for_filename(const std::string& s) {
	std::string out;
	out.reserve(s.size());
	for (char c : s) {
		out += (std::isalnum(static_cast<unsigned char>(c)) ? c : '_');
	}
	return out;
}

long load_process_convo_ttl_ms() {
	std::ifstream f("config/supervisor.json");
	if (!f.is_open()) {
		f.open("../config/supervisor.json");
	}
	if (!f.is_open()) {
		f.open("build/config/supervisor.json");
	}
	if (!f.is_open()) {
		return 86400L * 1000L;
	}
	try {
		json cfg;
		f >> cfg;
		const long ttl_sec =
		    cfg.value("conversation", json::object()).value("process_convo_ttl_sec", 86400L);
		return ttl_sec * 1000L;
	} catch (...) {
		return 86400L * 1000L;
	}
}

}  // namespace

// ── Constructor / Destructor ───────────────────────────────────────────────

ConversationManager::ConversationManager(const std::string& storage_path)
	: storage_path_(storage_path), stop_cleanup_(false) {
	try {
		fs::create_directories(storage_path_ + "/users");
		fs::create_directories(storage_path_ + "/proc");
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("Failed to create convo storage dirs: ") + e.what(),
		              "convo_mgr", "", -1, "storage_init_error");
	}
	cleanup_thread_ = std::thread([this] { cleanup_loop(); });
}

ConversationManager::~ConversationManager() {
	{
		std::lock_guard<std::mutex> lock(cleanup_mutex_);
		stop_cleanup_ = true;
	}
	cleanup_cv_.notify_all();
	if (cleanup_thread_.joinable()) {
		cleanup_thread_.join();
	}
}

// ── Path helpers ───────────────────────────────────────────────────────────

std::string ConversationManager::user_convo_path(const std::string& user_id) const {
	return storage_path_ + "/users/" + sanitize_for_filename(user_id) + ".json";
}

std::string ConversationManager::process_convo_path(int creator_pid,
                                                     const std::string& convo_id) const {
	return storage_path_ + "/proc/" + std::to_string(creator_pid) + "/" + convo_id + ".json";
}

std::string ConversationManager::convo_path_from_struct(const Conversation& convo) const {
	if (convo.convo_type == "user") {
		return user_convo_path(convo.user_id);
	}
	return process_convo_path(convo.creator_pid, convo.convo_id);
}

/**
 * Infer the disk path from the convo_id prefix alone.
 * user convos:    "user_{sanitized}" → users/{sanitized}.json
 * process convos: "proc_{pid}_{rand}" → proc/{pid}/proc_{pid}_{rand}.json
 */
std::string ConversationManager::infer_convo_path(const std::string& convo_id) const {
	if (convo_id.size() > 5 && convo_id.substr(0, 5) == "user_") {
		return storage_path_ + "/users/" + convo_id.substr(5) + ".json";
	}
	if (convo_id.size() > 5 && convo_id.substr(0, 5) == "proc_") {
		// Extract creator_pid: "proc_{pid}_{rand}" → find first '_' after "proc_"
		const std::string rest = convo_id.substr(5);
		const auto sep = rest.find('_');
		if (sep != std::string::npos) {
			const std::string pid_str = rest.substr(0, sep);
			return storage_path_ + "/proc/" + pid_str + "/" + convo_id + ".json";
		}
	}
	return "";  // Unknown format — caller must handle
}

// ── ID generators ─────────────────────────────────────────────────────────

std::string ConversationManager::generate_user_convo_id(const std::string& user_id) {
	// Deterministic: "user_{sanitized_user_id}" — no random suffix needed
	// because uniqueness is enforced by the one-per-user rule.
	return "user_" + sanitize_for_filename(user_id);
}

std::string ConversationManager::generate_process_convo_id(int creator_pid) {
	static thread_local std::mt19937 gen{std::random_device{}()};
	std::uniform_int_distribution<int> dis(100000, 999999);
	return "proc_" + std::to_string(creator_pid) + "_" + std::to_string(dis(gen));
}

// ── Disk I/O ───────────────────────────────────────────────────────────────

std::string ConversationManager::load_text_file(const std::string& path) const {
	std::ifstream file(path);
	if (!file.is_open()) {
		return "";
	}
	std::ostringstream buf;
	buf << file.rdbuf();
	return buf.str();
}

json ConversationManager::load_sampling_params() const {
	json defaults = {{"temp", 0.7}, {"top_p", 0.9}, {"max_tokens", 512}};
	std::ifstream f("config/model.json");
	if (!f.is_open()) {
		f.open("../config/model.json");
	}
	if (!f.is_open()) {
		f.open("build/config/model.json");
	}
	if (!f.is_open()) {
		return defaults;
	}
	try {
		json cfg;
		f >> cfg;
		if (cfg.contains("default_sampling_params") && cfg["default_sampling_params"].is_object()) {
			return cfg["default_sampling_params"];
		}
	} catch (...) {}
	return defaults;
}

Conversation ConversationManager::load_convo_from_path(const std::string& path) {
	Conversation nf;
	nf.creator_pid = -1;

	if (path.empty() || !fs::exists(path)) {
		return nf;
	}
	try {
		std::ifstream f(path);
		if (!f.is_open()) {
			return nf;
		}
		json j;
		f >> j;

		Conversation convo;
		convo.convo_id         = j.value("convo_id", "");
		convo.convo_type       = j.value("convo_type", "process");
		convo.user_id          = j.value("user_id", "");
		convo.creator_pid      = j.value("creator_pid", -1);
		convo.state            = j.value("state", "ACTIVE");
		convo.created_at_ms    = j.value("created_at_ms", 0L);
		convo.last_activity_ms = j.value("last_activity_ms", 0L);
		convo.turn_count       = j.value("turn_count", 0);
		convo.total_tokens_used = j.value("total_tokens_used", static_cast<uint64_t>(0));
		convo.metadata         = j.value("metadata", json::object());

		const json msgs = j.value("messages", json::array());
		if (msgs.is_array()) {
			for (const auto& msg : msgs) {
				if (msg.is_object()) {
					convo.messages.push_back(msg);
				}
			}
		}
		return convo;
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("Failed loading convo from ") + path + ": " + e.what(),
		              "convo_mgr", "", -1, "load_error");
		return nf;
	}
}

Conversation ConversationManager::load_conversation_from_disk_unlocked(const std::string& convo_id) {
	return load_convo_from_path(infer_convo_path(convo_id));
}

bool ConversationManager::persist_conversation(const Conversation& convo) {
	json j;
	j["convo_id"]          = convo.convo_id;
	j["convo_type"]        = convo.convo_type;
	j["user_id"]           = convo.user_id;
	j["creator_pid"]       = convo.creator_pid;
	j["state"]             = convo.state;
	j["created_at_ms"]     = convo.created_at_ms;
	j["last_activity_ms"]  = convo.last_activity_ms;
	j["turn_count"]        = convo.turn_count;
	j["total_tokens_used"] = convo.total_tokens_used;
	j["messages"]          = convo.messages;
	j["metadata"]          = convo.metadata;

	try {
		const std::string path = convo_path_from_struct(convo);
		fs::create_directories(fs::path(path).parent_path());
		std::ofstream out(path);
		if (!out.is_open()) {
			LOG_ERROR_CTX("Cannot open file for write: " + path,
			              "convo_mgr", "", convo.creator_pid, "file_open_error");
			return false;
		}
		out << j.dump(2);
		return true;
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("persist_conversation failed: ") + e.what(),
		              "convo_mgr", "", convo.creator_pid, "persist_error");
		return false;
	}
}

// ── CRUD ──────────────────────────────────────────────────────────────────

Conversation ConversationManager::get_or_create_user_convo(const std::string& user_id) {
	if (user_id.empty()) {
		throw std::runtime_error("user_id must not be empty for user conversations");
	}

	const std::string convo_id = generate_user_convo_id(user_id);
	const std::string path     = user_convo_path(user_id);

	std::lock_guard<std::mutex> lock(convo_mutex_);

	// Check cache
	auto cache_it = convo_cache_.find(convo_id);
	if (cache_it != convo_cache_.end()) {
		return cache_it->second;
	}

	// Check disk
	if (fs::exists(path)) {
		Conversation existing = load_convo_from_path(path);
		if (!existing.convo_id.empty()) {
			convo_cache_[convo_id] = existing;
			return existing;
		}
	}

	// Create new
	Conversation convo;
	convo.convo_id    = convo_id;
	convo.convo_type  = "user";
	convo.user_id     = user_id;
	convo.creator_pid = -1;  // user convos have no single creator pid
	convo.state       = "ACTIVE";

	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::system_clock::now().time_since_epoch()).count();
	convo.created_at_ms    = now_ms;
	convo.last_activity_ms = now_ms;

	persist_conversation(convo);
	convo_cache_[convo_id] = convo;
	LOG_INFO_CTX("Created user convo for user_id=" + user_id, "convo_mgr", "", -1, "create_user_convo");
	return convo;
}

Conversation ConversationManager::create_process_convo(int creator_pid) {
	if (creator_pid <= 0) {
		throw std::runtime_error("creator_pid must be positive for process conversations");
	}

	std::lock_guard<std::mutex> lock(convo_mutex_);

	Conversation convo;
	convo.convo_id    = generate_process_convo_id(creator_pid);
	convo.convo_type  = "process";
	convo.creator_pid = creator_pid;
	convo.state       = "ACTIVE";

	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::system_clock::now().time_since_epoch()).count();
	convo.created_at_ms    = now_ms;
	convo.last_activity_ms = now_ms;

	persist_conversation(convo);
	convo_cache_[convo.convo_id] = convo;
	LOG_INFO_CTX("Created process convo " + convo.convo_id,
	             "convo_mgr", "", creator_pid, "create_proc_convo");
	return convo;
}

Conversation ConversationManager::get_conversation(const std::string& convo_id) {
	std::lock_guard<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	if (it != convo_cache_.end()) {
		return it->second;
	}
	Conversation loaded = load_conversation_from_disk_unlocked(convo_id);
	if (!loaded.convo_id.empty()) {
		convo_cache_[convo_id] = loaded;
	}
	return loaded;
}

bool ConversationManager::append_message_unlocked(Conversation& convo,
                                                   const std::string& role,
                                                   const std::string& content,
												   uint64_t tokens_used,
												   const json& extra_fields) {
	const std::string canonical_role = canonicalize_role(role);
	const bool has_tool_calls =
	    extra_fields.is_object() && extra_fields.contains("tool_calls") &&
	    extra_fields["tool_calls"].is_array() &&
	    !extra_fields["tool_calls"].empty();
	if (!is_valid_role(canonical_role) || (content.empty() && !has_tool_calls)) {
		return false;
	}
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::system_clock::now().time_since_epoch()).count();
	json msg;
	msg["role"]         = canonical_role;
	msg["content"]      = content;
	if (extra_fields.is_object() && extra_fields.contains("tool_call_id") &&
	    extra_fields["tool_call_id"].is_string() &&
	    !extra_fields["tool_call_id"].get<std::string>().empty()) {
		msg["tool_call_id"] = extra_fields["tool_call_id"];
	}
	if (has_tool_calls) {
		msg["tool_calls"] = extra_fields["tool_calls"];
	}
	msg["timestamp_ms"] = now_ms;
	convo.messages.push_back(msg);
	convo.turn_count        += 1;
	convo.total_tokens_used += tokens_used;
	convo.last_activity_ms   = now_ms;
	return true;
}

bool ConversationManager::append_message(const std::string& convo_id,
                                         const std::string& role,
                                         const std::string& content,
                                         uint64_t tokens_used,
                                         const json& extra_fields) {
	std::lock_guard<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	Conversation convo = (it != convo_cache_.end())
	                     ? it->second
	                     : load_conversation_from_disk_unlocked(convo_id);
	if (convo.convo_id.empty()) {
		LOG_WARN_CTX("append_message: convo not found: " + convo_id,
		             "convo_mgr", "", -1, "not_found");
		return false;
	}
	if (!append_message_unlocked(convo, role, content, tokens_used, extra_fields)) {
		return false;
	}
	if (!persist_conversation(convo)) {
		return false;
	}
	convo_cache_[convo_id] = convo;
	return true;
}

bool ConversationManager::delete_conversation(const std::string& convo_id, int creator_pid) {
	std::lock_guard<std::mutex> lock(convo_mutex_);
	try {
		const std::string path = (creator_pid > 0)
		                         ? process_convo_path(creator_pid, convo_id)
		                         : infer_convo_path(convo_id);
		if (!path.empty() && fs::exists(path)) {
			fs::remove(path);
		}
		convo_cache_.erase(convo_id);
		return true;
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("delete_conversation failed: ") + e.what(),
		              "convo_mgr", "", creator_pid, "delete_error");
		return false;
	}
}

void ConversationManager::delete_process_convos_for_pid(int creator_pid) {
	{
		std::lock_guard<std::mutex> lock(convo_mutex_);
		for (auto it = convo_cache_.begin(); it != convo_cache_.end();) {
			if (it->second.creator_pid == creator_pid && it->second.convo_type == "process") {
				it = convo_cache_.erase(it);
			} else {
				++it;
			}
		}
	}
	const std::string pid_dir = storage_path_ + "/proc/" + std::to_string(creator_pid);
	try {
		if (fs::exists(pid_dir)) {
			fs::remove_all(pid_dir);
			LOG_INFO_CTX("Deleted process convo dir for pid " + std::to_string(creator_pid),
			             "convo_mgr", "", creator_pid, "delete_pid_dir");
		}
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("delete_process_convos_for_pid failed: ") + e.what(),
		              "convo_mgr", "", creator_pid, "delete_pid_dir_error");
	}
}

std::vector<std::string> ConversationManager::get_process_convos_for_pid(int creator_pid) {
	std::lock_guard<std::mutex> lock(convo_mutex_);
	std::vector<std::string> result;
	const std::string pid_dir = storage_path_ + "/proc/" + std::to_string(creator_pid);
	try {
		if (fs::exists(pid_dir)) {
			for (const auto& entry : fs::directory_iterator(pid_dir)) {
				if (entry.is_regular_file() && entry.path().extension() == ".json") {
					result.push_back(entry.path().stem().string());
				}
			}
		}
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("get_process_convos_for_pid failed: ") + e.what(),
		              "convo_mgr", "", creator_pid, "list_error");
	}
	return result;
}

bool ConversationManager::close_conversation(const std::string& convo_id) {
	std::lock_guard<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	if (it != convo_cache_.end()) {
		it->second.state = "CLOSED";
		persist_conversation(it->second);
		return true;
	}
	// Try loading from disk
	Conversation convo = load_conversation_from_disk_unlocked(convo_id);
	if (!convo.convo_id.empty()) {
		convo.state = "CLOSED";
		persist_conversation(convo);
		return true;
	}
	return false;
}

std::string ConversationManager::find_convo_for_user(const std::string& user_id) {
	if (user_id.empty()) {
		return "";
	}
	// O(1): derive path directly from user_id
	const std::string convo_id = generate_user_convo_id(user_id);
	const std::string path     = user_convo_path(user_id);
	if (fs::exists(path)) {
		return convo_id;
	}
	return "";
}

// ── Message builders ──────────────────────────────────────────────────────

json ConversationManager::build_simple_mode_messages(const std::string& user_message) {
	const std::string soul       = load_text_file("memory/soul.md");
	const std::string user_facts = load_text_file("memory/user.md");

	std::string sys;
	if (!soul.empty())       { sys += "Personality constraints:\n" + soul + "\n\n"; }
	if (!user_facts.empty()) { sys += "Known user facts:\n" + user_facts; }
	if (sys.empty())         { sys  = "You are velix assistant."; }

	json msgs = json::array();
	msgs.push_back({{"role", "system"}, {"content", sys}});
	msgs.push_back({{"role", "user"},   {"content", user_message}});
	return msgs;
}

json ConversationManager::build_conversation_messages_safely(const json& normalized_request) {
	const std::string convo_id = normalized_request.value("convo_id", "");
	if (convo_id.empty()) {
		throw std::runtime_error("convo_id missing when building messages");
	}

	const bool skip_compaction = normalized_request.value("metadata", json::object())
	                                               .value("compaction_request", false);

	struct PendingInput {
		std::string role;
		std::string content;
		json extra_fields = json::object();
	};
	std::vector<PendingInput> pending_inputs;

	if (normalized_request.contains("system_message")) {
		const std::string text = normalized_request.value("system_message", "");
		if (!text.empty()) {
			pending_inputs.push_back(PendingInput{"system", text, json::object()});
		}
	}

	if (normalized_request.contains("user_message")) {
		const std::string text = normalized_request.value("user_message", "");
		if (!text.empty()) {
			pending_inputs.push_back(PendingInput{"user", text, json::object()});
		}
	}

	if (normalized_request.contains("tool_message") &&
	    normalized_request["tool_message"].is_object()) {
		const json tool_msg = normalized_request["tool_message"];
		const std::string text = tool_msg.value("content", "");
		if (!text.empty()) {
			json extra = json::object();
			if (tool_msg.contains("tool_call_id") && tool_msg["tool_call_id"].is_string()) {
				extra["tool_call_id"] = tool_msg["tool_call_id"];
			}
			pending_inputs.push_back(PendingInput{"tool", text, extra});
		}
	}

	if (normalized_request.contains("tool_messages") &&
	    normalized_request["tool_messages"].is_array()) {
		for (const auto &tool_msg : normalized_request["tool_messages"]) {
			if (!tool_msg.is_object()) {
				continue;
			}
			const std::string text = tool_msg.value("content", "");
			if (text.empty()) {
				continue;
			}
			json extra = json::object();
			if (tool_msg.contains("tool_call_id") && tool_msg["tool_call_id"].is_string()) {
				extra["tool_call_id"] = tool_msg["tool_call_id"];
			}
			pending_inputs.push_back(PendingInput{"tool", text, extra});
		}
	}

	if (normalized_request.contains("tool_result")) {
		const std::string text = normalized_request.value("tool_result", "");
		if (!text.empty()) {
			json extra = json::object();
			if (normalized_request.contains("tool_call_id") &&
			    normalized_request["tool_call_id"].is_string()) {
				extra["tool_call_id"] = normalized_request["tool_call_id"];
			}
			pending_inputs.push_back(PendingInput{"tool", text, extra});
		}
	}

	if (pending_inputs.empty()) {
		throw std::runtime_error("conversation mode request missing input text");
	}

	Conversation convo;
	{
		std::lock_guard<std::mutex> lock(convo_mutex_);
		auto cache_it = convo_cache_.find(convo_id);
		convo = (cache_it != convo_cache_.end())
		            ? cache_it->second
		            : load_conversation_from_disk_unlocked(convo_id);
	}

	if (convo.convo_id.empty()) {
		throw std::runtime_error("conversation not found during build_messages");
	}

	json history = json::array();
	for (const auto& msg : convo.messages) {
		if (msg.is_object()) {
			history.push_back(msg);
		}
	}

	// Compaction guard: skip if this request itself came from the compacter.
	// We run this OUTSIDE the main convo_mutex_ so we don't block other conversations.
	if (!skip_compaction) {
		const std::string hist_path = convo_path_from_struct(convo);
		const CompactResult result  = compact_history_if_needed(history, hist_path);
		if (result.compacted) {
			convo.messages.clear();
			for (const auto& msg : result.history) {
				if (msg.is_object()) { convo.messages.push_back(msg); }
			}
			convo.turn_count = static_cast<int>(convo.messages.size());
		}
	}

	{
		std::lock_guard<std::mutex> lock(convo_mutex_);
		for (const auto &input : pending_inputs) {
			// Some providers (e.g., llama.cpp chat templates) require system
			// to appear only at the very beginning of the conversation.
			if (input.role == "system" && !convo.messages.empty()) {
				continue;
			}
			if (!append_message_unlocked(convo, input.role, input.content, 0,
			                            input.extra_fields)) {
				throw std::runtime_error("failed to append conversation input message");
			}
		}
		persist_conversation(convo);
		convo_cache_[convo_id] = convo;
	}

	json msgs = json::array();
	for (const auto& msg : convo.messages) {
		const std::string role    = canonicalize_role(msg.value("role", ""));
		const std::string content = msg.value("content", "");
		const bool has_tool_calls = msg.contains("tool_calls") &&
		                            msg["tool_calls"].is_array() &&
		                            !msg["tool_calls"].empty();
		if (!is_valid_role(role) || (content.empty() && !has_tool_calls)) { continue; }
		json out = {{"role", role}, {"content", content}};
		if (role == "assistant" && has_tool_calls) {
			out["tool_calls"] = msg["tool_calls"];
		}
		if (role == "tool" && msg.contains("tool_call_id") &&
		    msg["tool_call_id"].is_string() &&
		    !msg["tool_call_id"].get<std::string>().empty()) {
			out["tool_call_id"] = msg["tool_call_id"];
		}
		msgs.push_back(out);
	}
	return msgs;
}

// ── normalize_llm_request ─────────────────────────────────────────────────

json ConversationManager::normalize_llm_request(const json& raw_request) {
	std::lock_guard<std::mutex> lock(convo_mutex_);

	json normalized = raw_request;
	const std::string mode = raw_request.value("mode", "simple");



	// Fill in defaults
	if (normalized.value("request_id", "").empty()) {
		normalized["request_id"] = generate_request_id();
	}
	if (normalized.value("tree_id", "").empty()) {
		normalized["tree_id"] = "TREE_HANDLER";
	}
	if (!normalized.contains("source_pid")) { normalized["source_pid"] = -1; }
	if (!normalized.contains("priority"))   { normalized["priority"]   = 1; }
	if (!normalized.contains("sampling_params") || !normalized["sampling_params"].is_object()) {
		normalized["sampling_params"] = load_sampling_params();
	}

	if (mode == "conversation") {
		const int    source_pid = raw_request.value("source_pid", -1);
		std::string  convo_id   = raw_request.value("convo_id",  "");
		std::string  user_id    = raw_request.value("user_id",   "");

		Conversation convo;

		if (!user_id.empty()) {
			// Canonical user mode: one deterministic conversation per user.
			convo    = get_or_create_user_convo_unlocked(user_id);
			convo_id = convo.convo_id;
		} else if (!convo_id.empty() && convo_id.rfind("user_", 0) == 0) {
			// If only a user-prefixed convo_id is supplied, derive the user and map
			// to that user's canonical single conversation.
			const std::string inferred_user_id = convo_id.substr(5);
			if (inferred_user_id.empty()) {
				throw std::runtime_error("invalid user convo_id: missing user suffix");
			}
			convo    = get_or_create_user_convo_unlocked(inferred_user_id);
			convo_id = convo.convo_id;
			user_id  = convo.user_id;
		} else if (!convo_id.empty()) {
			// Bug #1: Validate that proc_ convo_id matches source_pid
			if (convo_id.rfind("proc_", 0) == 0 && source_pid > 0) {
				const size_t first_us = convo_id.find('_');
				const size_t second_us = convo_id.find('_', first_us + 1);
				if (first_us != std::string::npos && second_us != std::string::npos) {
					try {
						const int embedded_pid = std::stoi(convo_id.substr(first_us + 1, second_us - first_us - 1));
						if (embedded_pid != source_pid) {
							throw std::runtime_error("convo_id pid prefix (" + std::to_string(embedded_pid) + 
													 ") does not match source_pid (" + std::to_string(source_pid) + ")");
						}
					} catch (const std::exception&) {
						throw std::runtime_error("invalid convo_id PID format");
					}
				}
			}

			// Client supplied convo_id → load
			auto cache_it = convo_cache_.find(convo_id);
			if (cache_it != convo_cache_.end()) {
				convo = cache_it->second;
			} else {
				convo = load_conversation_from_disk_unlocked(convo_id);
				if (!convo.convo_id.empty()) {
					convo_cache_[convo_id] = convo;
				}
			}

			if (convo.convo_id.empty()) {
				// New process convo: create on the fly
				if (source_pid <= 0) {
					throw std::runtime_error(
					    "conversation not found and source_pid missing to create one");
				}
				convo             = Conversation{};
				convo.convo_id    = convo_id;
				convo.convo_type  = "process";
				convo.creator_pid = source_pid;
				convo.state       = "ACTIVE";
				const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
				    std::chrono::system_clock::now().time_since_epoch()).count();
				convo.created_at_ms    = now_ms;
				convo.last_activity_ms = now_ms;

				// Issue #4: explicitly persist upon first creation
				persist_conversation(convo);
			}

			user_id = convo.user_id;
		} else {
			throw std::runtime_error(
			    "conversation mode requires convo_id or user_id");
		}

		// Text validation is deferred to build_conversation_messages_safely inside the scheduler worker_loop
		// We just leave the request as is, except for ensuring convo_id is injected based on our lookup
		if (!raw_request.contains("messages") || !raw_request["messages"].is_array()) {
			normalized["convo_id"] = convo.convo_id;
			if (!raw_request.contains("tool_result") && !raw_request.contains("tool_message") &&
				!raw_request.contains("tool_messages") &&
				!raw_request.contains("user_message") && !raw_request.contains("system_message")) {
				throw std::runtime_error(
					"conversation mode requires messages[], user_message, system_message, tool_message(s), or tool_result");
			}
		}

		// Also update normalized with the final user_id and creator_pid for access validation by Supervisor
		normalized["user_id"] = convo.user_id;
		normalized["convo_type"] = convo.convo_type;

		convo_cache_[convo.convo_id] = convo;

		// Set fields the scheduler passes to the supervisor for access validation
		normalized["convo_id"]   = convo.convo_id;
		normalized["convo_type"] = convo.convo_type;
		normalized["user_id"]    = user_id;
		// Keep source_pid as-is — supervisor validates access using it

	} else {
		// Simple mode
		if (!raw_request.contains("messages") || !raw_request["messages"].is_array()) {
			const std::string user_msg = raw_request.value("user_message", "");
			if (user_msg.empty()) {
				throw std::runtime_error("simple mode requires messages[] or user_message");
			}
			normalized["messages"] = build_simple_mode_messages(user_msg);
		}
	}

	normalized["message_type"] = "LLM_REQUEST";
	normalized["mode"]         = mode;
	return normalized;
}

// Helper: get_or_create_user_convo without acquiring the lock (lock already held)
Conversation ConversationManager::get_or_create_user_convo_unlocked(const std::string& user_id) {
	// This is the same as get_or_create_user_convo but called while convo_mutex_ is held.
	// We cannot re-acquire the lock.
	const std::string convo_id = generate_user_convo_id(user_id);
	const std::string path     = user_convo_path(user_id);

	auto cache_it = convo_cache_.find(convo_id);
	if (cache_it != convo_cache_.end()) {
		return cache_it->second;
	}
	if (fs::exists(path)) {
		Conversation existing = load_convo_from_path(path);
		if (!existing.convo_id.empty()) {
			convo_cache_[convo_id] = existing;
			return existing;
		}
	}
	Conversation convo;
	convo.convo_id    = convo_id;
	convo.convo_type  = "user";
	convo.user_id     = user_id;
	convo.creator_pid = -1;
	convo.state       = "ACTIVE";
	const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::system_clock::now().time_since_epoch()).count();
	convo.created_at_ms    = now_ms;
	convo.last_activity_ms = now_ms;
	persist_conversation(convo);
	convo_cache_[convo_id] = convo;
	return convo;
}

bool ConversationManager::persist_assistant_response(const json& normalized_request,
                                                      const std::string& assistant_text) {
	if (assistant_text.empty()) { return true; }
	if (normalized_request.value("mode", "simple") != "conversation") { return true; }

	const std::string convo_id = normalized_request.value("convo_id", "");
	if (convo_id.empty()) { return false; }

	std::lock_guard<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	Conversation convo = (it != convo_cache_.end())
	                     ? it->second
	                     : load_conversation_from_disk_unlocked(convo_id);
	if (convo.convo_id.empty()) { return false; }

	if (!append_message_unlocked(convo, "assistant", assistant_text, 0)) { return false; }
	if (!persist_conversation(convo)) { return false; }
	convo_cache_[convo_id] = convo;
	return true;
}

bool ConversationManager::persist_assistant_tool_call(
	    const json& normalized_request, const std::string& assistant_text,
	    const json& tool_calls) {
	if (normalized_request.value("mode", "simple") != "conversation") {
		return true;
	}
	if (!tool_calls.is_array() || tool_calls.empty()) {
		return true;
	}

	const std::string convo_id = normalized_request.value("convo_id", "");
	if (convo_id.empty()) {
		return false;
	}

	std::lock_guard<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	Conversation convo = (it != convo_cache_.end())
	                     ? it->second
	                     : load_conversation_from_disk_unlocked(convo_id);
	if (convo.convo_id.empty()) {
		return false;
	}

	if (!append_message_unlocked(convo, "assistant", assistant_text, 0,
	                           {{"tool_calls", tool_calls}})) {
		return false;
	}
	if (!persist_conversation(convo)) {
		return false;
	}
	convo_cache_[convo_id] = convo;
	return true;
}

// ── Fix #2 — Background cleanup ───────────────────────────────────────────

void ConversationManager::cleanup_loop() {
	constexpr long interval_ms = 3600L * 1000L;  // every hour

	while (true) {
		{
			std::unique_lock<std::mutex> lock(cleanup_mutex_);
			cleanup_cv_.wait_for(lock, std::chrono::milliseconds(interval_ms),
			                     [this] { return stop_cleanup_.load(); });
			if (stop_cleanup_) { return; }
		}

		const long ttl_ms = load_process_convo_ttl_ms();
		const long now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
		    std::chrono::system_clock::now().time_since_epoch()).count();

		std::vector<int> stale_pids;
		const std::string proc_dir = storage_path_ + "/proc";

		try {
			if (!fs::exists(proc_dir)) { continue; }

			for (const auto& pid_entry : fs::directory_iterator(proc_dir)) {
				if (!pid_entry.is_directory()) { continue; }
				int pid = -1;
				try { pid = std::stoi(pid_entry.path().filename().string()); }
				catch (...) { continue; }

				long most_recent = 0;
				for (const auto& fe : fs::directory_iterator(pid_entry.path())) {
					if (!fe.is_regular_file() || fe.path().extension() != ".json") { continue; }
					try {
						std::ifstream f(fe.path().string());
						if (!f.is_open()) { continue; }
						json j;
						f >> j;
						const long act = j.value("last_activity_ms", 0L);
						if (act > most_recent) { most_recent = act; }
					} catch (...) {}
				}
				if (most_recent > 0 && (now_ms - most_recent) >= ttl_ms) {
					stale_pids.push_back(pid);
				}
			}
		} catch (const std::exception& e) {
			LOG_WARN_CTX(std::string("cleanup_loop scan error: ") + e.what(),
			             "convo_mgr", "", -1, "cleanup_scan_error");
		}

		for (int pid : stale_pids) {
			LOG_INFO_CTX("Cleanup: removing stale process convos for pid " + std::to_string(pid),
			             "convo_mgr", "", pid, "cleanup_stale");
			delete_process_convos_for_pid(pid);
		}
	}
}

}  // namespace velix::llm
