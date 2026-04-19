#include "session_io.hpp"
#include "session_manager.hpp"
#include "skills/skill_registry.hpp"
#include "storage/json_storage_provider.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <algorithm>
#include <random>
#include <regex>
#include <set>

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

std::uint64_t estimate_text_tokens(const std::string& text) {
	return static_cast<std::uint64_t>(text.size() / 4);
}

std::uint64_t estimate_history_tokens(const std::vector<json>& messages) {
	std::uint64_t total = 0;
	for (const auto& msg : messages) {
		if (!msg.is_object()) {
			continue;
		}
		total += estimate_text_tokens(msg.value("content", std::string("")));
	}
	return total;
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
	} catch (const std::exception &e) {
		// Failed to parse conversation config; use default TTL
		return 86400L * 1000L;
	}
}

// ── Prompt Sanity & Safety Helpers ────────────────────────────────────────

const char* DEFAULT_VELIX_IDENTITY =
    "You are Velix Agent, an intelligent AI assistant. You are helpful, knowledgeable, and direct. "
    "You assist users with a wide range of tasks including answering questions, writing and editing code, "
    "analyzing information, and executing actions via your tools. You communicate clearly, "
    "admit uncertainty when appropriate, and prioritize being genuinely useful over being verbose. "
    "Be targeted and efficient in your investigations.";

const std::vector<std::pair<std::string, std::string>> THREAT_PATTERNS = {
    {"ignore\\s+(previous|all|above|prior)\\s+instructions", "prompt_injection"},
    {"do\\s+not\\s+tell\\s+the\\s+user", "deception_hide"},
    {"system\\s+prompt\\s+override", "sys_prompt_override"},
    {"disregard\\s+(your|all|any)\\s+(instructions|rules|guidelines)", "disregard_rules"},
    {"act\\s+as\\s+(if|though)\\s+you\\s+(have\\s+no|don't\\s+have)\\s+(restrictions|limits|rules)", "bypass_restrictions"},
    {"<!--[^>]*(?:ignore|override|system|secret|hidden)[^>]*-->", "html_comment_injection"},
    {"<\\s*div\\s+style\\s*=\\s*[\"'].*display\\s*:\\s*none", "hidden_div"},
    {"translate\\s+.*\\s+into\\s+.*\\s+and\\s+(execute|run|eval)", "translate_execute"},
    {"curl\\s+[^\\n]*\\$\\{?\\w*(KEY|TOKEN|SECRET|PASSWORD|CREDENTIAL|API)", "exfil_curl"},
    {"cat\\s+[^\\n]*(\\.env|credentials|\\.netrc|\\.pgpass)", "read_secrets"}
};

// Zero-width and other "invisible" characters used for injection
const std::set<std::string> INVISIBLE_CHARS = {
    "\u200b", "\u200c", "\u200d", "\u2060", "\ufeff",
    "\u202a", "\u202b", "\u202c", "\u202d", "\u202e"
};

std::string clean_invisible_chars(std::string content) {
	for (const auto& ch : INVISIBLE_CHARS) {
		size_t pos = 0;
		while ((pos = content.find(ch, pos)) != std::string::npos) {
			content.erase(pos, ch.length());
		}
	}
	return content;
}

std::string strip_yaml_frontmatter(const std::string& content) {
	if (content.substr(0, 3) == "---") {
		auto end = content.find("\n---", 3);
		if (end != std::string::npos) {
			std::string body = content.substr(end + 4);
			// Trim leading newlines
			size_t first = body.find_first_not_of("\n\r");
			if (first != std::string::npos) {
				return body.substr(first);
			}
			return body;
		}
	}
	return content;
}

std::string scan_content_for_threats(const std::string& content, const std::string& filename) {
	for (const auto& [pattern, pid] : THREAT_PATTERNS) {
		try {
			std::regex re(pattern, std::regex_constants::icase);
			if (std::regex_search(content, re)) {
				LOG_WARN_CTX("Threat detected in " + filename + ": " + pid, "prompt_safety", "", -1, "blocked_content");
				return "[BLOCKED: " + filename + " contained potential prompt injection (" + pid + "). Content not loaded.]";
			}
		} catch (const std::regex_error&) {
			// Regex matching failed; skip threat check for this pattern
		}
	}
	return content;
}

std::string truncate_content(const std::string& content, const std::string& filename, size_t max_chars = 20000) {
	if (content.size() <= max_chars) {
		return content;
	}
	size_t head_chars = static_cast<size_t>(max_chars * 0.7);
	size_t tail_chars = static_cast<size_t>(max_chars * 0.2);
	std::string head = content.substr(0, head_chars);
	std::string tail = content.substr(content.size() - tail_chars);
	std::string marker = "\n\n[...truncated " + filename + ": kept " + std::to_string(head_chars) + "+" + 
	                     std::to_string(tail_chars) + " of " + std::to_string(content.size()) + 
	                     " chars. Use file tools to read the full file.]\n\n";
	return head + marker + tail;
}

std::string process_context_file(const std::string& content, const std::string& filename) {
	if (content.empty()) return "";
	
	std::string cleaned = clean_invisible_chars(content);
	std::string safe = scan_content_for_threats(cleaned, filename);
	
	// If it was blocked, don't do further processing
	if (safe.substr(0, 9) == "[BLOCKED:") return safe;
	
	std::string stripped = strip_yaml_frontmatter(safe);
	return truncate_content(stripped, filename);
}

}  // namespace

// ── Constructor / Destructor ───────────────────────────────────────────────

SessionIO::SessionIO(const std::string& storage_path)
	: SessionIO(std::make_shared<storage::JsonStorageProvider>(storage_path),
	            storage_path) {}

SessionIO::SessionIO(std::shared_ptr<storage::IStorageProvider> storage,
	                 const std::string& storage_path)
	: storage_path_(storage_path), storage_(std::move(storage)), stop_cleanup_(false) {
	try {
		fs::create_directories(storage_path_ + "/users");
		fs::create_directories(storage_path_ + "/procs");
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("Failed to create convo storage dirs: ") + e.what(),
		              "convo_mgr", "", -1, "storage_init_error");
	}
	cleanup_thread_ = std::thread([this] { cleanup_loop(); });
}

SessionIO::~SessionIO() {
	{
		std::scoped_lock<std::mutex> lock(cleanup_mutex_);
		stop_cleanup_ = true;
	}
	cleanup_cv_.notify_all();
	if (cleanup_thread_.joinable()) {
		cleanup_thread_.join();
	}
}

// ── Path helpers ───────────────────────────────────────────────────────────

std::string SessionIO::user_convo_path(const std::string& session_id) const {
	const auto super_user = SessionManager::extract_super_user(session_id);
	
	const auto session_dir = storage_path_ + "/users/" + super_user + "/" + session_id;
	try {
		std::filesystem::create_directories(session_dir);
	} catch (const std::filesystem::filesystem_error&) {
		// Failed to create directory; session_dir may not be writable
	}
	
	return session_dir + "/" + session_id + ".json";
}

std::string SessionIO::process_convo_path(int creator_pid,
                                          const std::string& convo_id) const {
	const std::string session_dir = storage_path_ + "/procs/" + std::to_string(creator_pid) + "/" + convo_id;
	try {
		std::filesystem::create_directories(session_dir);
	} catch (const std::filesystem::filesystem_error&) {
		// Failed to create directory; session_dir may not be writable
	}
	return session_dir + "/" + convo_id + ".json";
}

std::string SessionIO::convo_path_from_struct(const Conversation& convo) const {
	if (convo.creator_pid <= 0) {
		return user_convo_path(convo.convo_id);
	}
	return process_convo_path(convo.creator_pid, convo.convo_id);
}

/**
 * Infer the disk path from session id / process id naming conventions.
 */
std::string SessionIO::infer_convo_path(const std::string& convo_id) const {
	if (SessionManager::is_session_id(convo_id)) {
		return user_convo_path(convo_id);
	}
	if (convo_id.size() > 5 && convo_id.substr(0, 5) == "proc_") {
		// Extract creator_pid: "proc_{pid}_{rand}" → find first '_' after "proc_"
		const std::string rest = convo_id.substr(5);
		const auto sep = rest.find('_');
		if (sep != std::string::npos) {
			const std::string pid_str = rest.substr(0, sep);
			return process_convo_path(std::stoi(pid_str), convo_id);
		}
	}
	return "";  // Unknown format — caller must handle
}

// ── ID generators ─────────────────────────────────────────────────────────

std::string SessionIO::generate_user_convo_id(const std::string& user_id) {
	if (user_id.empty()) {
		return "";
	}
	if (SessionManager::is_session_id(user_id)) {
		return user_id;
	}
	// Strict mode: user conversations must always use explicit session ids.
	return "";
}

std::string SessionIO::generate_process_convo_id(int creator_pid) {
	static thread_local std::mt19937 gen{std::random_device{}()};
	std::uniform_int_distribution<int> dis(100000, 999999);
	return "proc_" + std::to_string(creator_pid) + "_" + std::to_string(dis(gen));
}

// ── Disk I/O ───────────────────────────────────────────────────────────────

std::string SessionIO::load_text_file(const std::string& path) const {
	std::ifstream file(path);
	if (!file.is_open()) {
		return "";
	}
	std::ostringstream buf;
	buf << file.rdbuf();
	return buf.str();
}

// ── Static helpers ─────────────────────────────────────────────────────────

std::string SessionIO::load_text_with_fallbacks(const std::vector<std::string>& paths) {
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
  *   3. Skills menu         (available SKILL.md descriptions)
  *   4. caller_system_msg   (appended, never replaces the base layers)
  *
  * The caller_system_msg parameter replaces what used to silently disappear
  * when the SDK passed an empty string and the scheduler omitted the field.
  * Non-empty caller messages are now always appended after the base layers.
  */
std::string SessionIO::build_layered_system_prompt(
    const std::string& mode,
    const std::string& caller_system_msg,
    const std::string& user_id) {

	// Centralize session-id parsing so all components honor the same _s{N} rule.
	const std::string super_user = SessionManager::extract_super_user(user_id);

	const std::string general_guidelines_raw = load_text_with_fallbacks({
	    "memory/general_guidelines.md",
	    "../memory/general_guidelines.md",
	    "build/memory/general_guidelines.md"
	});
	const std::string general_guidelines = process_context_file(general_guidelines_raw, "general_guidelines.md");

	// Soul (persona) — per-super-user first, global fallback.
	const std::string soul_raw = (mode == "user_conversation")
	    ? load_text_with_fallbacks({
	          "memory/agentfiles/" + super_user + "/soul.md",
	          "memory/soul.md",
	          "../memory/soul.md",
	          "build/memory/soul.md"
	      })
	    : std::string("");
	const std::string soul = process_context_file(soul_raw, "soul.md");

	// user.md — per-super-user facts, only for user_conversation.
	const std::string user_facts_raw = (mode == "user_conversation")
	    ? load_text_with_fallbacks({
	          "memory/agentfiles/" + super_user + "/user.md",
	          "memory/user.md",
	          "../memory/user.md",
	          "build/memory/user.md"
	      })
	    : std::string("");
	const std::string user_facts = process_context_file(user_facts_raw, "user.md");

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
	
	// Inject skills menu after user facts, before caller message
	// Format: VELIX KNOWLEDGE DIRECTORY for optimal LLM discoverability
	try {
		velix::llm::skills::SkillRegistry skill_registry;
		json skills_menu = skill_registry.get_skills_menu();
		if (skills_menu.is_array() && !skills_menu.empty()) {
			if (!combined.empty()) combined += "\n\n";
			combined += "VELIX KNOWLEDGE DIRECTORY\n";
			combined += "You may call skill_view(skill_name=\"NAME\") only when the task needs step-by-step or procedural instructions not already in the conversation or system prompt. Load a skill once per task: call skill_view(name, mode=\"summary\") to inspect; if more detail is needed, call mode=\"body\". Do NOT follow skill instructions unless you explicitly loaded the skill with skill_view.\n\n";
			for (const auto& skill : skills_menu) {
				std::string name = skill.value("name", "");
				std::string desc = skill.value("description", "");
				if (!name.empty()) {
					combined += "- " + name + ": " + desc + "\n";
				}
			}
			combined += "\nNOTE: If you request an unavailable skill, the tool may return helpful nearest matches.";
		}
	} catch (const std::exception &e) {
		// Silently ignore skill loading errors; system prompt assembly should not fail
	}
	
	if (!caller_system_msg.empty()) {
		if (!combined.empty()) combined += "\n\n";
		// Also scan caller system msg for threats, but don't truncate (usually short)
		combined += scan_content_for_threats(caller_system_msg, "caller_system_message");
	}
	if (combined.empty()) {
		combined = DEFAULT_VELIX_IDENTITY;
	}
	return combined;
}


json SessionIO::load_sampling_params() const {
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
	} catch (const std::exception &e) {
		// Failed to parse model.json or load sampling params; use defaults
	}
	return defaults;
}

Conversation SessionIO::load_convo_from_path(const std::string& path) {
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
		convo.current_context_tokens = j.value("current_context_tokens", static_cast<uint64_t>(0));
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

Conversation SessionIO::load_conversation_from_disk_unlocked(const std::string& convo_id) {
	if (!storage_) {
		return load_convo_from_path(infer_convo_path(convo_id));
	}
	const auto row = storage_->get_conversation(convo_id);
	if (!row.has_value() || !row->is_object()) {
		Conversation nf;
		nf.creator_pid = -1;
		return nf;
	}
	const json& j = *row;
	Conversation convo;
	convo.convo_id = j.value("convo_id", "");
	convo.user_id = j.value("user_id", "");
	convo.creator_pid = j.value("creator_pid", -1);
	convo.state = j.value("state", "ACTIVE");
	convo.created_at_ms = j.value("created_at_ms", 0L);
	convo.last_activity_ms = j.value("last_activity_ms", 0L);
	convo.turn_count = j.value("turn_count", 0);
	convo.current_context_tokens = j.value("current_context_tokens", static_cast<uint64_t>(0));
	convo.total_tokens_used = j.value("total_tokens_used", static_cast<uint64_t>(0));
	convo.metadata = j.value("metadata", json::object());
	const json msgs = j.value("messages", json::array());
	if (msgs.is_array()) {
		for (const auto& msg : msgs) {
			if (msg.is_object()) {
				convo.messages.push_back(msg);
			}
		}
	}
	return convo;
}

bool SessionIO::persist_conversation(const Conversation& convo) {
	json j;
	j["convo_id"]          = convo.convo_id;
	j["user_id"]           = convo.user_id;
	j["creator_pid"]       = convo.creator_pid;
	j["state"]             = convo.state;
	j["created_at_ms"]     = convo.created_at_ms;
	j["last_activity_ms"]  = convo.last_activity_ms;
	j["turn_count"]        = convo.turn_count;
	j["current_context_tokens"] = convo.current_context_tokens;
	j["total_tokens_used"] = convo.total_tokens_used;
	j["messages"]          = convo.messages;
	j["metadata"]          = convo.metadata;

	try {
		if (storage_) {
			return storage_->upsert_conversation(j);
		}
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

Conversation SessionIO::get_or_create_user_convo(const std::string& user_id) {
	if (user_id.empty()) {
		throw std::runtime_error("user_id must not be empty for user conversations");
	}

	const std::string session_id = generate_user_convo_id(user_id);
	if (session_id.empty()) {
		throw std::runtime_error("user_conversation requires user_id in session format '{super_user}_s{N}'");
	}
	const std::string convo_id = session_id;
	const std::string path     = user_convo_path(session_id);

	std::scoped_lock<std::mutex> lock(convo_mutex_);

	// Check cache
	auto cache_it = convo_cache_.find(convo_id);
	if (cache_it != convo_cache_.end()) {
		return cache_it->second;
	}

	// Check disk (filesystem JSON)
	if (fs::exists(path)) {
		Conversation existing = load_convo_from_path(path);
		if (!existing.convo_id.empty()) {
			convo_cache_[convo_id] = existing;
			return existing;
		}
	}

	// Check storage provider (SQLite if configured)
	if (storage_) {
		Conversation existing = load_conversation_from_disk_unlocked(convo_id);
		if (!existing.convo_id.empty()) {
			convo_cache_[convo_id] = existing;
			LOG_INFO_CTX("Loaded existing user convo from storage for user_id=" + user_id, 
			             "convo_mgr", "", -1, "load_user_convo");
			return existing;
		}
	}

	// Create new
	Conversation convo;
	convo.convo_id    = convo_id;
	convo.user_id     = SessionManager::extract_super_user(session_id);
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

Conversation SessionIO::create_process_convo(int creator_pid) {
	if (creator_pid <= 0) {
		throw std::runtime_error("creator_pid must be positive for process conversations");
	}

	std::scoped_lock<std::mutex> lock(convo_mutex_);

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

Conversation SessionIO::get_conversation(const std::string& convo_id) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);
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

void SessionIO::invalidate_conversation_cache(const std::string& convo_id) {
	if (convo_id.empty()) {
		return;
	}
	std::scoped_lock<std::mutex> lock(convo_mutex_);
	convo_cache_.erase(convo_id);
}

bool SessionIO::append_message_unlocked(Conversation& convo,
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
	const std::uint64_t token_delta =
	    (tokens_used > 0) ? tokens_used : estimate_text_tokens(content);
	convo.turn_count        += 1;
	convo.current_context_tokens += token_delta;
	convo.total_tokens_used += token_delta;
	convo.last_activity_ms   = now_ms;
	return true;
}

bool SessionIO::append_message(const std::string& convo_id,
                                         const std::string& role,
                                         const std::string& content,
                                         uint64_t tokens_used,
                                         const json& extra_fields) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);
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

bool SessionIO::delete_conversation(const std::string& convo_id, int creator_pid) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);
	try {
		if (storage_) {
			if (!storage_->delete_conversation(convo_id, creator_pid)) {
				return false;
			}
		} else {
			const std::string path = (creator_pid > 0)
			                         ? process_convo_path(creator_pid, convo_id)
			                         : infer_convo_path(convo_id);
			if (!path.empty() && fs::exists(path)) {
				fs::remove(path);
			}
		}
		convo_cache_.erase(convo_id);
		return true;
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("delete_conversation failed: ") + e.what(),
		              "convo_mgr", "", creator_pid, "delete_error");
		return false;
	}
}

void SessionIO::delete_process_convos_for_pid(int creator_pid) {
	{
		std::scoped_lock<std::mutex> lock(convo_mutex_);
		for (auto it = convo_cache_.begin(); it != convo_cache_.end();) {
			if (it->second.creator_pid == creator_pid) {
				it = convo_cache_.erase(it);
			} else {
				++it;
			}
		}
	}
	if (storage_) {
		storage_->delete_all_proc_convos(creator_pid);
		return;
	}
	const std::string pid_dir = storage_path_ + "/procs/" + std::to_string(creator_pid);
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

std::vector<std::string> SessionIO::get_process_convos_for_pid(int creator_pid) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);
	if (storage_) {
		return storage_->list_proc_convo_ids(creator_pid);
	}
	std::vector<std::string> result;
	const std::string pid_dir = storage_path_ + "/procs/" + std::to_string(creator_pid);
	try {
		if (fs::exists(pid_dir)) {
			for (const auto& entry : fs::directory_iterator(pid_dir)) {
				if (entry.is_directory()) {
					result.push_back(entry.path().filename().string());
				}
			}
		}
	} catch (const std::exception& e) {
		LOG_ERROR_CTX(std::string("get_process_convos_for_pid failed: ") + e.what(),
		              "convo_mgr", "", creator_pid, "list_error");
	}
	return result;
}

bool SessionIO::close_conversation(const std::string& convo_id) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);
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

std::string SessionIO::find_convo_for_user(const std::string& user_id) {
	if (user_id.empty()) {
		return "";
	}
	// O(1): derive path directly from user_id
	const std::string convo_id = generate_user_convo_id(user_id);
	const std::string path     = user_convo_path(convo_id);
	if (fs::exists(path)) {
		return convo_id;
	}
	return "";
}

// ── Message builders ──────────────────────────────────────────────────────

json SessionIO::build_simple_mode_messages(
    const std::string& user_message,
    const std::string& caller_system_msg) {

	const std::string sys = build_layered_system_prompt("simple", caller_system_msg);

	json msgs = json::array();
	msgs.push_back({{"role", "system"}, {"content", sys}});
	msgs.push_back({{"role", "user"},   {"content", user_message}});
	return msgs;
}


json SessionIO::build_conversation_messages_safely(const json& normalized_request) {
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
		std::scoped_lock<std::mutex> lock(convo_mutex_);
		auto cache_it = convo_cache_.find(convo_id);
		convo = (cache_it != convo_cache_.end())
		            ? cache_it->second
		            : load_conversation_from_disk_unlocked(convo_id);
	}

	if (convo.convo_id.empty()) {
		throw std::runtime_error("conversation not found during build_messages");
	}

	{
		std::scoped_lock<std::mutex> lock(convo_mutex_);
		if (!explicit_system_prompt.empty()) {
			if (system_prompt_override) {
				// Clear old system messages from history
				convo.messages.erase(
				    std::remove_if(convo.messages.begin(), convo.messages.end(),
				                   [](const json& msg) {
					                   return msg.is_object() && msg.value("role", "") == "system";
				                   }),
				    convo.messages.end());

				// Add new system message to history (ephemeral, not stored in metadata)
				if (!append_message_unlocked(convo, "system", explicit_system_prompt, 0,
				                            json::object())) {
					throw std::runtime_error("failed to set overriding system prompt");
				}
			} else if (convo.messages.empty()) {
				// First turn - add system message to history (ephemeral, not stored in metadata)
				if (!append_message_unlocked(convo, "system", explicit_system_prompt, 0,
				                            json::object())) {
					throw std::runtime_error("failed to append initial system prompt");
				}
			}
			// Note: System prompts are ephemeral - not stored in metadata
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
	// explicit_system_prompt (if provided) is appended after the base layers.
	// If a system message is already in the persisted history we use that;
	// otherwise we synthesise a fresh one so every API call is self-contained.

	bool history_has_system = false;
	for (const auto& msg : convo.messages) {
		if (msg.is_object() && msg.value("role", "") == "system") {
			history_has_system = true;
			break;
		}
	}

	if (!history_has_system) {
		// No system message yet in history, synthesise the full layered one.
		// Use explicit_system_prompt (per-request, ephemeral).
		const std::string full_sys =
		    build_layered_system_prompt(mode, explicit_system_prompt, convo.user_id);
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

json SessionIO::normalize_llm_request(const json& raw_request) {
	std::scoped_lock<std::mutex> lock(convo_mutex_);

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
		const std::string resolved_session_id = generate_user_convo_id(user_id);
		if (resolved_session_id.empty()) {
			throw std::runtime_error("failed to resolve a valid session_id for user_conversation");
		}

		Conversation convo = get_or_create_user_convo_unlocked(resolved_session_id);

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
		normalized["user_id"] = resolved_session_id;
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
Conversation SessionIO::get_or_create_user_convo_unlocked(const std::string& user_id) {
	// This is the same as get_or_create_user_convo but called while convo_mutex_ is held.
	// We cannot re-acquire the lock.
	const std::string session_id = generate_user_convo_id(user_id);
	if (session_id.empty()) {
		throw std::runtime_error("user_conversation requires user_id in session format '{super_user}_s{N}'");
	}
	const std::string convo_id = session_id;
	const std::string path     = user_convo_path(session_id);

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

	// Check storage provider (SQLite if configured)
	if (storage_) {
		Conversation existing = load_conversation_from_disk_unlocked(convo_id);
		if (!existing.convo_id.empty()) {
			convo_cache_[convo_id] = existing;
			return existing;
		}
	}

	// Create new only if not found anywhere
	Conversation convo;
	convo.convo_id    = convo_id;
	convo.user_id     = SessionManager::extract_super_user(session_id);
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

bool SessionIO::persist_assistant_response(const json& normalized_request,
                                                      const std::string& assistant_text,
                                                      uint64_t tokens_used) {
	if (assistant_text.empty()) { return true; }
	const std::string mode = normalized_request.value("mode", "simple");
	if (mode != "conversation" && mode != "user_conversation") { return true; }

	const std::string convo_id = normalized_request.value("convo_id", "");
	if (convo_id.empty()) { return false; }

	std::scoped_lock<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	Conversation convo = (it != convo_cache_.end())
	                     ? it->second
	                     : load_conversation_from_disk_unlocked(convo_id);
	if (convo.convo_id.empty()) { return false; }

	if (!append_message_unlocked(convo, "assistant", assistant_text, tokens_used)) { return false; }
	if (!persist_conversation(convo)) { return false; }
	convo_cache_[convo_id] = convo;
	return true;
}

bool SessionIO::persist_assistant_tool_call(
	    const json& normalized_request, const std::string& assistant_text,
	    const json& tool_calls, uint64_t tokens_used) {
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

	std::scoped_lock<std::mutex> lock(convo_mutex_);
	auto it = convo_cache_.find(convo_id);
	Conversation convo = (it != convo_cache_.end())
	                     ? it->second
	                     : load_conversation_from_disk_unlocked(convo_id);
	if (convo.convo_id.empty()) {
		return false;
	}

	if (!append_message_unlocked(convo, "assistant", assistant_text, tokens_used,
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

void SessionIO::cleanup_loop() {
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
		if (storage_) {
			stale_pids = storage_->list_proc_creator_pids();
			for (int pid : stale_pids) {
				const auto convos = storage_->list_proc_convo_ids(pid);
				long most_recent = 0;
				for (const auto &convo_id : convos) {
					auto convo_row = storage_->get_conversation(convo_id);
					if (!convo_row.has_value() || !convo_row->is_object()) {
						continue;
					}
					const long act = convo_row->value("last_activity_ms", 0L);
					if (act > most_recent) {
						most_recent = act;
					}
				}
				if (most_recent > 0 && (now_ms - most_recent) >= ttl_ms) {
					LOG_INFO_CTX("Cleanup: removing stale process convos for pid " +
					             std::to_string(pid),
					             "convo_mgr", "", pid, "cleanup_stale");
					delete_process_convos_for_pid(pid);
				}
			}
			continue;
		}

		const std::string proc_dir = storage_path_ + "/procs";

		try {
			if (!fs::exists(proc_dir)) { continue; }

			for (const auto& pid_entry : fs::directory_iterator(proc_dir)) {
				if (!pid_entry.is_directory()) { continue; }
				int pid = -1;
				try { pid = std::stoi(pid_entry.path().filename().string()); }
				catch (const std::exception &e) { continue; }

				long most_recent = 0;
				for (const auto& session_entry : fs::directory_iterator(pid_entry.path())) {
					if (!session_entry.is_directory()) { continue; }
					for (const auto& fe : fs::directory_iterator(session_entry.path())) {
						if (!fe.is_regular_file() || fe.path().extension() != ".json") { continue; }
						const std::string stem = fe.path().stem().string();
						if (stem.find("_h") != std::string::npos) { continue; }
						try {
							std::ifstream f(fe.path().string());
							if (!f.is_open()) { continue; }
							json j;
							f >> j;
							const long act = j.value("last_activity_ms", 0L);
							if (act > most_recent) { most_recent = act; }
						} catch (const std::exception &e) {
							// Failed to parse JSON file; skip it
						}
					}
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
