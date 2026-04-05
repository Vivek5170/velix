#include "conversation_manager.hpp"
#include "compacter.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <algorithm>
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
	// Session-based user_ids (e.g. "terminal_vivek_s1") store their live
	// conversation inside memory/sessions/{user_id}/{user_id}.json so that
	// SessionManager can co-locate snapshots and the live file in one folder.
	// Check for this layout first; fall back to the legacy flat path.
	const std::string session_path =
	    "memory/sessions/" + user_id + "/" + user_id + ".json";
	if (std::filesystem::exists(session_path) ||
	    std::filesystem::exists("memory/sessions/" + user_id)) {
		// Ensure the directory exists for new sessions.
		try {
			std::filesystem::create_directories("memory/sessions/" + user_id);
		} catch (...) {}
		return session_path;
	}
	// Legacy / non-session path.
	return storage_path_ + "/users/" + sanitize_for_filename(user_id) + ".json";
}

std::string ConversationManager::process_convo_path(int creator_pid,
                                                     const std::string& convo_id) const {
	return storage_path_ + "/proc/" + std::to_string(creator_pid) + "/" + convo_id + ".json";
}

std::string ConversationManager::convo_path_from_struct(const Conversation& convo) const {
	if (convo.creator_pid <= 0) {
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
		return user_convo_path(convo_id.substr(5));
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

// ── Static helpers ─────────────────────────────────────────────────────────

std::string ConversationManager::load_text_with_fallbacks(const std::vector<std::string>& paths) {
	for (const auto& path : paths) {
		std::ifstream f(path);
		if (!f.is_open()) { continue; }
		std::ostringstream buf;
		buf << f.rdbuf();
		return buf.str();
	}
	return "";
}

/**
 * build_layered_system_prompt — single source of truth for system prompt assembly.
 *
 * Layer order (each non-empty piece is separated by "\n\n"):
 *   1. General guidelines  (memory/general_guidelines.md)
 *   2. Soul / personality  (memory/soul.md — only for user_conversation)
 *   3. caller_system_msg   (appended, never replaces the base layers)
 *
 * The caller_system_msg parameter replaces what used to silently disappear
 * when the SDK passed an empty string and the scheduler omitted the field.
 * Non-empty caller messages are now always appended after the base layers.
 */
std::string ConversationManager::build_layered_system_prompt(
    const std::string& mode,
    const std::string& caller_system_msg,
    const std::string& user_id) {

	// Extract the super_user from the session_id (e.g. "terminal_vivek_s1"
	// → "terminal_vivek") so we can load per-user soul.md and user.md.
	// If user_id has no _s{N} suffix it is already the super_user.
	std::string super_user = user_id;
	if (!user_id.empty()) {
		const auto pos = user_id.rfind("_s");
		if (pos != std::string::npos && pos + 2 < user_id.size()) {
			const std::string suffix = user_id.substr(pos + 2);
			if (!suffix.empty()) super_user = user_id.substr(0, pos);
		}
	}

	const std::string general_guidelines = load_text_with_fallbacks({
	    "memory/general_guidelines.md",
	    "../memory/general_guidelines.md",
	    "build/memory/general_guidelines.md"
	});

	// Soul (persona) — per-super-user first, global fallback.
	const std::string soul = (mode == "user_conversation" || mode == "simple")
	    ? load_text_with_fallbacks({
	          "memory/agentfiles/" + super_user + "/soul.md",
	          "memory/soul.md",
	          "../memory/soul.md",
	          "build/memory/soul.md"
	      })
	    : std::string("");

	// user.md — per-super-user facts, only for user_conversation.
	const std::string user_facts = (mode == "user_conversation")
	    ? load_text_with_fallbacks({
	          "memory/agentfiles/" + super_user + "/user.md",
	          "memory/user.md",
	          "../memory/user.md",
	          "build/memory/user.md"
	      })
	    : std::string("");

	std::string combined;
	if (!general_guidelines.empty()) {
		combined += "General guidelines:\n" + general_guidelines;
	}
	if (!soul.empty()) {
		if (!combined.empty()) combined += "\n\n";
		combined += "Personality constraints:\n" + soul;
	}
	if (!user_facts.empty()) {
		if (!combined.empty()) combined += "\n\n";
		combined += "Known user facts:\n" + user_facts;
	}
	if (!caller_system_msg.empty()) {
		if (!combined.empty()) combined += "\n\n";
		combined += caller_system_msg;
	}
	if (combined.empty()) {
		combined = "You are Velix Agent, an intelligent AI assistant.";
	}
	return combined;
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
			if (it->second.creator_pid == creator_pid) {
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

json ConversationManager::build_simple_mode_messages(
    const std::string& user_message,
    const std::string& caller_system_msg) {

	const std::string sys = build_layered_system_prompt("simple", caller_system_msg);

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
	const std::string mode = normalized_request.value("mode", "user_conversation");

	// Extract caller-supplied system message. This is APPENDED to guidelines+soul,
	// not used to replace them. Both "system_prompt" and "system_message" are honoured.
	const std::string explicit_system_prompt = [&]() {
		if (normalized_request.contains("system_prompt") &&
		    normalized_request["system_prompt"].is_string()) {
			return normalized_request["system_prompt"].get<std::string>();
		}
		if (normalized_request.contains("system_message") &&
		    normalized_request["system_message"].is_string()) {
			return normalized_request["system_message"].get<std::string>();
		}
		return std::string("");
	}();

	const json metadata = normalized_request.value("metadata", json::object());
	const bool system_prompt_override =
	    normalized_request.value("system_prompt_override", false) ||
	    normalized_request.value("replace_system_prompt", false) ||
	    metadata.value("system_prompt_override", false);

	const bool skip_compaction = normalized_request.value("metadata", json::object())
	                                               .value("compaction_request", false);

	struct PendingInput {
		std::string role;
		std::string content;
		json extra_fields = json::object();
	};
	std::vector<PendingInput> pending_inputs;

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
		if (!explicit_system_prompt.empty()) {
			if (!convo.metadata.is_object()) {
				convo.metadata = json::object();
			}

			if (system_prompt_override) {
				convo.metadata["system_prompt"] = explicit_system_prompt;
				convo.messages.erase(
				    std::remove_if(convo.messages.begin(), convo.messages.end(),
				                   [](const json& msg) {
					                   return msg.is_object() && msg.value("role", "") == "system";
				                   }),
				    convo.messages.end());

				if (!append_message_unlocked(convo, "system", explicit_system_prompt, 0,
				                            json::object())) {
					throw std::runtime_error("failed to set overriding system prompt");
				}
			} else if (convo.messages.empty()) {
				convo.metadata["system_prompt"] = explicit_system_prompt;
				if (!append_message_unlocked(convo, "system", explicit_system_prompt, 0,
				                            json::object())) {
					throw std::runtime_error("failed to append initial system prompt");
				}
			} else if (!convo.metadata.contains("system_prompt") ||
			           !convo.metadata["system_prompt"].is_string() ||
			           convo.metadata["system_prompt"].get<std::string>().empty()) {
				convo.metadata["system_prompt"] = explicit_system_prompt;
			}
		}

		for (const auto &input : pending_inputs) {
			if (!append_message_unlocked(convo, input.role, input.content, 0,
			                            input.extra_fields)) {
				throw std::runtime_error("failed to append conversation input message");
			}
		}
		persist_conversation(convo);
		convo_cache_[convo_id] = convo;
	}

	json msgs = json::array();
	// ── System message ─────────────────────────────────────────────────────
	// Build via build_layered_system_prompt so soul+guidelines are always first.
	// The stored_system_prompt (per-conversation custom persona set at start)
	// is treated as a caller-supplied extension appended after the base layers.
	// If a system message is already in the persisted history we use that;
	// otherwise we synthesise a fresh one so every API call is self-contained.
	const std::string stored_system_prompt =
	    (convo.metadata.is_object() && convo.metadata.contains("system_prompt") &&
	     convo.metadata["system_prompt"].is_string())
	        ? convo.metadata["system_prompt"].get<std::string>()
	        : std::string("");

	bool history_has_system = false;
	for (const auto& msg : convo.messages) {
		if (msg.is_object() && msg.value("role", "") == "system") {
			history_has_system = true;
			break;
		}
	}

	if (!history_has_system) {
		// No system message yet in history, synthesise the full layered one.
		// Merge stored_system_prompt (conversation persona) + explicit_system_prompt
		// (per-turn override). Both are appended after soul+guidelines.
		std::string extra_for_prompt = stored_system_prompt;
		if (!explicit_system_prompt.empty() && explicit_system_prompt != stored_system_prompt) {
			if (!extra_for_prompt.empty()) extra_for_prompt += "\n\n";
			extra_for_prompt += explicit_system_prompt;
		}
		const std::string full_sys =
		    build_layered_system_prompt(mode, extra_for_prompt, convo.user_id);
		msgs.push_back({{"role", "system"}, {"content", full_sys}});
	}

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
	const std::string mode = raw_request.value("mode", "");

	// Security-sensitive fields are validated by scheduler; no fallback defaults here.
	if (!normalized.contains("priority"))   { normalized["priority"]   = 1; }
	if (!normalized.contains("sampling_params") || !normalized["sampling_params"].is_object()) {
		normalized["sampling_params"] = load_sampling_params();
	}

	if (mode == "conversation") {
		const int source_pid = raw_request.value("source_pid", -1);
		std::string convo_id = raw_request.value("convo_id", "");
		const std::string user_id = raw_request.value("user_id", "");

		if (!user_id.empty()) {
			throw std::runtime_error("conversation mode requires empty user_id");
		}
		if (source_pid <= 0) {
			throw std::runtime_error("conversation mode requires positive source_pid");
		}

		Conversation convo;
		if (convo_id.empty()) {
			convo.convo_id = generate_process_convo_id(source_pid);
			convo.creator_pid = source_pid;
			convo.state = "ACTIVE";
			const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
			    std::chrono::system_clock::now().time_since_epoch()).count();
			convo.created_at_ms = now_ms;
			convo.last_activity_ms = now_ms;
			persist_conversation(convo);
			convo_id = convo.convo_id;
		} else {
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
				throw std::runtime_error("conversation not found for provided convo_id");
			}
			if (convo.creator_pid != source_pid) {
				throw std::runtime_error("process conversation is owned by a different source_pid");
			}
		}

		if (!raw_request.contains("messages") || !raw_request["messages"].is_array()) {
			if (!raw_request.contains("tool_result") && !raw_request.contains("tool_message") &&
				!raw_request.contains("tool_messages") &&
				!raw_request.contains("user_message") && !raw_request.contains("system_message")) {
				throw std::runtime_error(
					"conversation mode requires messages[], user_message, system_message, tool_message(s), or tool_result");
			}
		}

		normalized["convo_id"] = convo.convo_id;
		normalized["user_id"] = "";
		convo_cache_[convo.convo_id] = convo;
	} else if (mode == "user_conversation") {
		const int source_pid = raw_request.value("source_pid", -1);
		const bool is_handler = raw_request.value("is_handler", false);
		const std::string convo_id = raw_request.value("convo_id", "");
		const std::string user_id = raw_request.value("user_id", "");

		if (source_pid <= 0) {
			throw std::runtime_error("user_conversation mode requires positive source_pid");
		}
		if (!is_handler) {
			throw std::runtime_error("user_conversation mode is allowed only for handler process");
		}
		if (user_id.empty()) {
			throw std::runtime_error("user_conversation mode requires non-empty user_id");
		}

		Conversation convo = get_or_create_user_convo_unlocked(user_id);

		if (!raw_request.contains("messages") || !raw_request["messages"].is_array()) {
			if (!raw_request.contains("tool_result") && !raw_request.contains("tool_message") &&
				!raw_request.contains("tool_messages") &&
				!raw_request.contains("user_message") && !raw_request.contains("system_message")) {
				throw std::runtime_error(
					"user_conversation mode requires messages[], user_message, system_message, tool_message(s), or tool_result");
			}
		}
		if (!convo_id.empty() && convo_id!=convo.convo_id) {
			throw std::runtime_error("conversation_id does not belong to said user");
		}

		normalized["convo_id"] = convo.convo_id;
		normalized["user_id"] = user_id;
		convo_cache_[convo.convo_id] = convo;

	} else if (mode == "simple") {
		// Simple mode
		const std::string convo_id = raw_request.value("convo_id", "");
		const std::string user_id = raw_request.value("user_id", "");
		if (!convo_id.empty() || !user_id.empty()) {
			throw std::runtime_error("simple mode requires empty convo_id and empty user_id");
		}
		if (!raw_request.contains("messages") || !raw_request["messages"].is_array()) {
			const std::string user_msg = raw_request.value("user_message", "");
			if (user_msg.empty()) {
				throw std::runtime_error("simple mode requires messages[] or user_message");
			}
			normalized["messages"] = build_simple_mode_messages(user_msg);
		}
	} else {
		throw std::runtime_error("unsupported mode: " + mode);
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
	const std::string mode = normalized_request.value("mode", "simple");
	if (mode != "conversation" && mode != "user_conversation") { return true; }

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
	const std::string mode = normalized_request.value("mode", "simple");
	if (mode != "conversation" && mode != "user_conversation") {
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
