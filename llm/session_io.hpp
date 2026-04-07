#pragma once

#include "../vendor/nlohmann/json.hpp"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cstdint>

using json = nlohmann::json;

namespace velix::llm {

// ── Conversation struct ────────────────────────────────────────────────────
//
// Two types:
//   "user"    – session-based user context addressed by session_id.
//               Storage: memory/sessions/users/{super_user}/{session_id}/{session_id}.json
//
//   "process" – ephemeral, bound to creator_pid; only that pid can use it.
//               Storage: memory/sessions/procs/{creator_pid}/{session_id}/{session_id}.json
//
struct Conversation {
	std::string convo_id;
	std::string user_id;       // only for type="user" – the unique user identifier
	int creator_pid = -1;      // for type="process": the only pid allowed to use this convo
	std::string state = "ACTIVE";  // "ACTIVE" | "CLOSED"

	long created_at_ms = 0;
	long last_activity_ms = 0;

	int turn_count = 0;
	uint64_t current_context_tokens = 0;
	uint64_t total_tokens_used = 0;
	std::vector<json> messages;      // [{role, content, timestamp_ms}, ...]
	json metadata = json::object();  // reserved for future per-convo policy
};

// ── SessionIO ───────────────────────────────────────────────────

class SessionIO {
public:
	explicit SessionIO(const std::string& storage_path = "memory/sessions");
	~SessionIO();

	// ── Request normalisation ─────────────────────────────────────────────

	/**
	 * Normalise an incoming LLM request so the scheduler can process it uniformly.
	 *
	 * Supported modes (strict, non-overlapping):
	 *   { mode:"simple", user_id:"", convo_id:"" }
	 *       → stateless request
	 *   { mode:"conversation", user_id:"", convo_id:""|"proc_N_XXXXX", source_pid:N }
	 *       → process conversation; empty convo_id creates a new process convo
	 *   { mode:"user_conversation", user_id:"terminal_vivek_s1", convo_id:"", source_pid:N, tree_id:"TREE_HANDLER" }
	 *       → user session conversation keyed by the provided session_id
	 *
	 * Always sets resolved convo_id and user_id on the normalised output so the
	 * scheduler can pass them to the supervisor for access validation.
	 */
	json normalize_llm_request(const json& raw_request);

	/**
	 * Build the layered system prompt for a given mode.
	 *
	 * Assembly order (all sections separated by double newline):
	 *   1. General guidelines   (memory/general_guidelines.md — always)
	 *   2. Soul / personality   (memory/soul.md — only for user_conversation)
	 *   3. caller_system_msg    (anything passed via system_message field — appended, never replaces)
	 *
	 * Returns the combined string, or an empty string if all sources are empty.
	 */
	static std::string build_layered_system_prompt(
	    const std::string& mode,
	    const std::string& caller_system_msg = "",
	    const std::string& user_id = "");

	/**
	 * Persist the assistant reply into a conversation.
	 * No-op for simple mode.
	 */
	bool persist_assistant_response(const json& normalized_request,
	                                const std::string& assistant_text,
	                                uint64_t tokens_used = 0);

	/**
	 * Persist an assistant turn that includes tool_calls. Content may be empty.
	 */
	bool persist_assistant_tool_call(const json& normalized_request,
	                                 const std::string& assistant_text,
	                                 const json& tool_calls,
	                                 uint64_t tokens_used = 0);

	// ── CRUD ──────────────────────────────────────────────────────────────

	/**
	 * Find or create the user conversation for user_id (idempotent).
	 * Returns the existing conversation if one already exists on disk.
	 */
	Conversation get_or_create_user_convo(const std::string& user_id);

	/**
	 * Create a new ephemeral process conversation for creator_pid.
	 * Only the creator can use it.
	 */
	Conversation create_process_convo(int creator_pid);

	/** Load a conversation by convo_id (from cache or disk). */
	Conversation get_conversation(const std::string& convo_id);

	/** Evict a conversation from in-memory cache so next access reloads from disk. */
	void invalidate_conversation_cache(const std::string& convo_id);

	/** Append a message; persists to disk. */
	bool append_message(const std::string& convo_id, const std::string& role,
	                    const std::string& content, uint64_t tokens_used = 0,
	                    const json& extra_fields = json::object());

	/** Persist a conversation object to disk. */
	bool persist_conversation(const Conversation& convo);

	/** Delete a single conversation file and evict from cache. */
	bool delete_conversation(const std::string& convo_id, int creator_pid);

	/** Delete all process-owned conversations for creator_pid (removes proc/{pid}/ dir). */
	void delete_process_convos_for_pid(int creator_pid);

	/** List all convo_ids stored for a given creator_pid (process-type only). */
	std::vector<std::string> get_process_convos_for_pid(int creator_pid);

	/** Mark a conversation CLOSED and persist. */
	bool close_conversation(const std::string& convo_id);

	/**
	 * Find the convo_id for the user conversation associated with user_id.
	 * O(1) – derives the path directly from user_id.
	 * Returns "" if not found.
	 */
	std::string find_convo_for_user(const std::string& user_id);

	/**
	 * Build messages array for conversation mode safely, handling lock properly
	 * around compaction steps. This is called by the scheduler worker thread.
	 */
	json build_conversation_messages_safely(const json& normalized_request);

private:
	std::string storage_path_;
	std::mutex convo_mutex_;

	std::unordered_map<std::string, Conversation> convo_cache_;

	// Background cleanup thread (process convos only)
	std::atomic<bool> stop_cleanup_;
	std::thread cleanup_thread_;
	std::mutex cleanup_mutex_;
	std::condition_variable cleanup_cv_;

	// ── Internal path helpers ─────────────────────────────────────────────
	std::string user_convo_path(const std::string& user_id) const;
	std::string process_convo_path(int creator_pid, const std::string& convo_id) const;
	std::string convo_path_from_struct(const Conversation& convo) const;

	/** Infer storage path from session_id/convo_id naming conventions. */
	std::string infer_convo_path(const std::string& convo_id) const;

	std::string generate_process_convo_id(int creator_pid);
	std::string generate_user_convo_id(const std::string& user_id);

	Conversation load_convo_from_path(const std::string& path);
	Conversation load_conversation_from_disk_unlocked(const std::string& convo_id);

	json load_sampling_params() const;
	std::string load_text_file(const std::string& path) const;

	json build_simple_mode_messages(const std::string& user_message,
	                                const std::string& caller_system_msg = "");

	/** Load a text file from several fallback paths relative to CWD. */
	static std::string load_text_with_fallbacks(const std::vector<std::string>& paths);



	bool append_message_unlocked(Conversation& convo, const std::string& role,
	                             const std::string& content, uint64_t tokens_used,
	                             const json& extra_fields = json::object());

	/** Lock-free variant called while convo_mutex_ is already held. */
	Conversation get_or_create_user_convo_unlocked(const std::string& user_id);

	/** Background: hourly cleanup of stale process-convo directories. */
	void cleanup_loop();
};

}  // namespace velix::llm
