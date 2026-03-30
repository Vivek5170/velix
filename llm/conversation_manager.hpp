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
//   "user"    – permanent, one per user_id, owned by no specific pid.
//               Only processes in TREE_HANDLER can use them.
//               Storage: memory/conversations/users/{sanitized_user_id}.json
//
//   "process" – ephemeral, bound to creator_pid; only that pid can use it.
//               Storage: memory/conversations/proc/{creator_pid}/{convo_id}.json
//
struct Conversation {
	std::string convo_id;
	std::string convo_type;    // "user" | "process"
	std::string user_id;       // only for type="user" – the unique user identifier
	int creator_pid = -1;      // for type="process": the only pid allowed to use this convo
	std::string state = "ACTIVE";  // "ACTIVE" | "CLOSED"

	long created_at_ms = 0;
	long last_activity_ms = 0;

	int turn_count = 0;
	uint64_t total_tokens_used = 0;

	std::vector<json> messages;      // [{role, content, timestamp_ms}, ...]
	json metadata = json::object();  // reserved for future per-convo policy
};

// ── ConversationManager ───────────────────────────────────────────────────

class ConversationManager {
public:
	explicit ConversationManager(const std::string& storage_path = "memory/conversations");
	~ConversationManager();

	// ── Request normalisation ─────────────────────────────────────────────

	/**
	 * Normalise an incoming LLM request so the scheduler can process it uniformly.
	 *
	 * Conversation mode forms accepted:
	 *   { mode:"conversation", user_id:"alice", source_pid:N }
	 *       → finds or creates the user convo for alice (idempotent)
	 *   { mode:"conversation", convo_id:"user_alice", source_pid:N }
	 *       → loads the user convo by id (any TREE_HANDLER pid may use it)
	 *   { mode:"conversation", convo_id:"proc_N_XXXXX", source_pid:N }
	 *       → loads or creates a process-owned temp convo
	 *
	 * Always sets convo_type and user_id on the normalised output so the
	 * scheduler can pass them to the supervisor for access validation.
	 */
	json normalize_llm_request(const json& raw_request);

	/**
	 * Persist the assistant reply into a conversation.
	 * No-op for simple mode.
	 */
	bool persist_assistant_response(const json& normalized_request,
	                                const std::string& assistant_text);

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

	/** Append a message; persists to disk. */
	bool append_message(const std::string& convo_id, const std::string& role,
	                    const std::string& content, uint64_t tokens_used = 0);

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

	/** Infer storage path from convo_id prefix ("user_" vs "proc_"). */
	std::string infer_convo_path(const std::string& convo_id) const;

	std::string generate_process_convo_id(int creator_pid);
	std::string generate_user_convo_id(const std::string& user_id);

	Conversation load_convo_from_path(const std::string& path);
	Conversation load_conversation_from_disk_unlocked(const std::string& convo_id);

	json load_sampling_params() const;
	std::string load_text_file(const std::string& path) const;

	json build_simple_mode_messages(const std::string& user_message);



	bool append_message_unlocked(Conversation& convo, const std::string& role,
	                             const std::string& content, uint64_t tokens_used);

	/** Lock-free variant called while convo_mutex_ is already held. */
	Conversation get_or_create_user_convo_unlocked(const std::string& user_id);

	/** Background: hourly cleanup of stale process-convo directories. */
	void cleanup_loop();
};

}  // namespace velix::llm
