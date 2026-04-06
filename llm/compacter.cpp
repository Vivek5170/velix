#include "compacter.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>

namespace velix::llm {

namespace {

struct CompacterConfig {
	bool enabled{true};
	std::size_t keep_recent_turns{16};
	std::size_t max_history_tokens_before_compact{3000};
	std::size_t summary_max_tokens{300};
	double summary_temp{0.2};
	double summary_top_p{0.9};
	int summary_priority{2};
	int scheduler_timeout_ms{30000};
	std::string summary_tree_id{"TREE_HANDLER"};
	int summary_source_pid{1000};
};

std::string now_iso8601() {
	const auto now = std::chrono::system_clock::now();
	const std::time_t now_time = std::chrono::system_clock::to_time_t(now);

	std::tm tm_buf{};
#ifdef _WIN32
	gmtime_s(&tm_buf, &now_time);
#else
	gmtime_r(&now_time, &tm_buf);
#endif

	std::ostringstream oss;
	oss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");
	return oss.str();
}

std::string random_id(const std::string& prefix) {
	static thread_local std::mt19937_64 rng{std::random_device{}()};
	const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
	const auto r = rng();
	std::ostringstream oss;
	oss << prefix << '-' << std::hex << ticks << '-' << (r & 0xFFFFFFFFULL);
	return oss.str();
}

CompacterConfig load_compacter_config() {
	CompacterConfig cfg;

	std::ifstream in("config/compacter.json");
	if (!in.is_open()) {
		in.open("../config/compacter.json");
	}
	if (!in.is_open()) {
		in.open("build/config/compacter.json");
	}
	if (!in.is_open()) {
		LOG_WARN("config/compacter.json not found, using fallback defaults");
		return cfg;
	}

	try {
		nlohmann::json j;
		in >> j;
		cfg.enabled = j.value("enabled", cfg.enabled);
		cfg.keep_recent_turns = j.value("keep_recent_turns", cfg.keep_recent_turns);
		cfg.max_history_tokens_before_compact = j.value("max_history_tokens_before_compact", cfg.max_history_tokens_before_compact);
		cfg.summary_max_tokens = j.value("summary_max_tokens", cfg.summary_max_tokens);
		cfg.summary_temp = j.value("summary_temp", cfg.summary_temp);
		cfg.summary_top_p = j.value("summary_top_p", cfg.summary_top_p);
		cfg.summary_priority = j.value("summary_priority", cfg.summary_priority);
		cfg.scheduler_timeout_ms = j.value("scheduler_timeout_ms", cfg.scheduler_timeout_ms);
		cfg.summary_tree_id = j.value("summary_tree_id", cfg.summary_tree_id);
		cfg.summary_source_pid = j.value("summary_source_pid", cfg.summary_source_pid);
	} catch (const std::exception& e) {
		LOG_ERROR(std::string("Failed parsing config/compacter.json: ") + e.what());
	}

	// Compaction requests must carry a valid positive source pid to satisfy
	// scheduler/supervisor validation.
	if (cfg.summary_source_pid <= 0) {
		cfg.summary_source_pid = 1000;
	}

	if (cfg.keep_recent_turns == 0) {
		cfg.keep_recent_turns = 1;
	}
	if (cfg.max_history_tokens_before_compact == 0) {
		cfg.max_history_tokens_before_compact = 1;
	}

	return cfg;
}

int load_scheduler_port() {
	std::ifstream ports_file("config/ports.json");
	if (!ports_file.is_open()) {
		ports_file.open("../config/ports.json");
	}
	if (!ports_file.is_open()) {
		ports_file.open("build/config/ports.json");
	}
	if (!ports_file.is_open()) {
		return 5171;
	}

	try {
		nlohmann::json ports;
		ports_file >> ports;
		return ports.value("LLM_SCHEDULER", 5171);
	} catch (...) {
		return 5171;
	}
}

std::size_t estimate_tokens_from_text(const std::string& text) {
	std::size_t tokens = 0;
	bool in_word = false;

	for (char ch : text) {
		const unsigned char c = static_cast<unsigned char>(ch);
		if (std::isalnum(c) || ch == '_') {
			if (!in_word) {
				++tokens;
				in_word = true;
			}
		} else {
			in_word = false;
			if (!std::isspace(c)) {
				++tokens;
			}
		}
	}

	return tokens;
}

std::size_t estimate_history_tokens(const nlohmann::json& history) {
	if (!history.is_array()) {
		return 0;
	}

	std::size_t token_total = 0;
	for (const auto& turn : history) {
		if (!turn.is_object()) {
			continue;
		}

		if (turn.contains("content") && turn["content"].is_string()) {
			token_total += estimate_tokens_from_text(turn["content"].get<std::string>());
		}
		if (turn.contains("role") && turn["role"].is_string()) {
			token_total += 2;
		}
	}

	return token_total;
}

std::string serialize_turns_for_summary_prompt(const nlohmann::json& older_turns) {
	std::ostringstream body;
	for (const auto& turn : older_turns) {
		if (!turn.is_object()) {
			continue;
		}
		if (!turn.contains("role") || !turn["role"].is_string()) {
			continue;
		}
		if (!turn.contains("content") || !turn["content"].is_string()) {
			continue;
		}

		body << '[';
		if (turn.contains("timestamp") && turn["timestamp"].is_string()) {
			body << turn["timestamp"].get<std::string>();
		} else {
			body << "unknown-time";
		}
		body << "] " << turn["role"].get<std::string>() << ": "
			 << turn["content"].get<std::string>() << "\n";
	}

	return body.str();
}

void persist_compacted_history(const std::string& history_file_path, const nlohmann::json& compacted) {
	std::ofstream history_out(history_file_path);
	if (!history_out.is_open()) {
		throw std::runtime_error("Failed opening history file for compaction write: " + history_file_path);
	}

	history_out << compacted.dump(2);
}

const std::string SUMMARY_PREFIX =
    "[CONTEXT COMPACTION] Earlier turns in this conversation were compacted "
    "to save context space. The summary below describes work that was "
    "already completed, and the current session state may still reflect "
    "that work (for example, files may already be changed). Use the summary "
    "and the current state to continue from where things left off, and "
    "avoid repeating work:\n\n";

const std::string LEGACY_SUMMARY_PREFIX = "[CONTEXT SUMMARY]:";

std::string request_llm_summary(
    const nlohmann::json& older_turns,
    const CompacterConfig& cfg,
    int scheduler_port
) {
    std::string previous_summary;
    nlohmann::json turns_to_process = nlohmann::json::array();

    // Detect if the first message is a previous summary to handle incremental compaction
    if (!older_turns.empty() && older_turns[0].is_object()) {
        const std::string first_content = older_turns[0].value("content", "");
        if (first_content.rfind(SUMMARY_PREFIX, 0) == 0) {
            previous_summary = first_content.substr(SUMMARY_PREFIX.size());
        } else if (first_content.rfind(LEGACY_SUMMARY_PREFIX, 0) == 0) {
            previous_summary = first_content.substr(LEGACY_SUMMARY_PREFIX.size());
        }
    }

    // Skip the previous summary in the list of "new turns" to summarize
    for (std::size_t i = (previous_summary.empty() ? 0 : 1); i < older_turns.size(); ++i) {
        turns_to_process.push_back(older_turns[i]);
    }

    const std::string content_to_summarize = serialize_turns_for_summary_prompt(turns_to_process);
    std::ostringstream prompt;

    if (!previous_summary.empty()) {
        // Incremental update prompt
        prompt << "Update the following technical handoff summary by incorporating the new conversation turns.\n\n"
               << "PREVIOUS SUMMARY:\n" << previous_summary << "\n\n"
               << "NEW TURNS TO INCORPORATE:\n" << content_to_summarize << "\n\n"
               << "Update the summary using this exact structure. PRESERVE all existing information that is still relevant. "
               << "ADD new progress. Move items from \"In Progress\" to \"Done\" when completed. Remove information only if it is clearly obsolete.\n\n"
               << "## Goal\n[What the user is trying to accomplish — preserve from previous summary, update if goal evolved]\n\n"
               << "## Constraints & Preferences\n[User preferences, coding style, constraints, important decisions — accumulate across compactions]\n\n"
               << "## Progress\n### Done\n[Completed work — include specific file paths, commands run, results obtained]\n"
               << "### In Progress\n[Work currently underway]\n"
               << "### Blocked\n[Any blockers or issues encountered]\n\n"
               << "## Key Decisions\n[Important technical decisions and why they were made]\n\n"
               << "## Relevant Files\n[Files read, modified, or created — with brief note on each. Accumulate across compactions.]\n\n"
               << "## Next Steps\n[What needs to happen next to continue the work]\n\n"
               << "## Critical Context\n[Any specific values, error messages, configuration details, or data that would be lost without explicit preservation]\n\n"
               << "Target ~" << cfg.summary_max_tokens << " tokens. Be specific — include file paths, command outputs, error messages, and concrete values rather than vague descriptions.\n\n"
               << "Write only the summary body. Do not include any preamble or prefix.";
    } else {
        // First compaction: summarize from scratch
        prompt << "Create a structured technical handoff summary for a later assistant that will continue this conversation after earlier turns are compacted.\n\n"
               << "TURNS TO SUMMARIZE:\n" << content_to_summarize << "\n\n"
               << "Use this exact structure:\n\n"
               << "## Goal\n[What the user is trying to accomplish]\n\n"
               << "## Constraints & Preferences\n[User preferences, coding style, constraints, important decisions]\n\n"
               << "## Progress\n### Done\n[Completed work — include specific file paths, commands run, results obtained]\n"
               << "### In Progress\n[Work currently underway]\n"
               << "### Blocked\n[Any blockers or issues encountered]\n\n"
               << "## Key Decisions\n[Important technical decisions and why they were made]\n\n"
               << "## Relevant Files\n[Files read, modified, or created — with brief note on each]\n\n"
               << "## Next Steps\n[What needs to happen next to continue the work]\n\n"
               << "## Critical Context\n[Any specific values, error messages, configuration details, or data that would be lost without explicit preservation]\n\n"
               << "Target ~" << cfg.summary_max_tokens << " tokens. Be specific — include file paths, command outputs, error messages, and concrete values rather than vague descriptions. "
               << "The goal is to prevent the next assistant from repeating work or losing important details.\n\n"
               << "Write only the summary body. Do not include any preamble or prefix.";
    }

    nlohmann::json req = {
        {"message_type", "LLM_REQUEST"},
        {"request_id", random_id("COMPACT")},
        {"tree_id", cfg.summary_tree_id},
        {"source_pid", cfg.summary_source_pid},
        {"mode", "simple"},
        {"convo_id", ""},
        {"user_id", ""},
        {"priority", std::numeric_limits<int>::max()},
        {"inherit_key", nullptr},
        {"messages", nlohmann::json::array({
            {{"role", "system"}, {"content", "You are an expert technical summarizer that preserves project state across context compactions."}},
            {{"role", "user"}, {"content", prompt.str()}}
        })},
        {"sampling_params", {
            {"temp", cfg.summary_temp},
            {"top_p", cfg.summary_top_p},
            {"max_tokens", cfg.summary_max_tokens}
        }},
        {"metadata", {
            {"request_origin", "compacter"},
            {"compaction_request", true},
            {"timestamp", now_iso8601()}
        }}
    };

    velix::communication::SocketWrapper socket;
    socket.create_tcp_socket();
    socket.connect(
        velix::communication::resolve_service_host("SCHEDULER", "127.0.0.1"),
        static_cast<uint16_t>(scheduler_port));
    socket.set_timeout_ms(cfg.scheduler_timeout_ms);

    velix::communication::send_json(socket, req.dump());
    const std::string response_payload = velix::communication::recv_json(socket);

    auto response = nlohmann::json::parse(response_payload);
    if (response.value("status", "error") != "ok") {
        throw std::runtime_error("Scheduler summarization failed: " + response.value("error", "unknown"));
    }

    const std::string summary = response.value("response", "");
    if (summary.empty()) {
        throw std::runtime_error("Scheduler summarization returned empty summary");
    }

    return summary;
}

void copy_metadata_from_older(const nlohmann::json& older_turns, nlohmann::json& summary_message) {
	for (auto iter = older_turns.rbegin(); iter != older_turns.rend(); ++iter) {
		if (!iter->is_object()) {
			continue;
		}
		if (iter->contains("timestamp") && (*iter)["timestamp"].is_string()) {
			summary_message["timestamp"] = (*iter)["timestamp"];
		}
		if (iter->contains("metadata") && (*iter)["metadata"].is_object()) {
			summary_message["metadata"] = (*iter)["metadata"];
		}
		break;
	}
}

} // namespace

CompactResult compact_history_if_needed(const nlohmann::json& history) {
	return compact_history_if_needed(history, "");
}

CompactResult compact_history_if_needed(const nlohmann::json& history, const std::string& history_file_override) {
	CompactResult result;
	const CompacterConfig cfg = load_compacter_config();

	if (!history.is_array()) {
		result.history = nlohmann::json::array();
		result.skip_reason = "invalid_history";
		return result;
	}

	result.history = history;
	if (!cfg.enabled) {
		result.skip_reason = "disabled";
		return result;
	}

	const std::size_t total_tokens = estimate_history_tokens(history);
	if (total_tokens <= cfg.max_history_tokens_before_compact || history.size() <= cfg.keep_recent_turns + 1) {
		if (history.size() <= cfg.keep_recent_turns + 1) {
			result.skip_reason = "below_preserve_limit";
		} else {
			result.skip_reason = "below_token_threshold";
		}
		return result;
	}

	const std::size_t split_index = history.size() - cfg.keep_recent_turns;

	nlohmann::json older = nlohmann::json::array();
	nlohmann::json recent = nlohmann::json::array();

	for (std::size_t index = 0; index < history.size(); ++index) {
		if (index < split_index) {
			older.push_back(history[index]);
		} else {
			recent.push_back(history[index]);
		}
	}

	std::string llm_summary;
	try {
		const int scheduler_port = load_scheduler_port();
		llm_summary = request_llm_summary(older, cfg, scheduler_port);
	} catch (const std::exception& e) {
		LOG_WARN(std::string("Compaction skipped because summary generation failed: ") + e.what());
		result.skip_reason = "summary_generation_failed";
		return result;
	}

	nlohmann::json compacted = nlohmann::json::array();
	nlohmann::json summary_message = {
		{"role", "assistant"},
		{"content", SUMMARY_PREFIX + llm_summary}
	};
	copy_metadata_from_older(older, summary_message);
	if (!summary_message.contains("timestamp")) {
		summary_message["timestamp"] = now_iso8601();
	}

	compacted.push_back(summary_message);

	for (const auto& turn : recent) {
		compacted.push_back(turn);
	}

	try {
		if (!history_file_override.empty()) {
			persist_compacted_history(history_file_override, compacted);
		}
	} catch (const std::exception& e) {
		LOG_WARN(std::string("Compaction summary created but persistence failed: ") + e.what());
		result.skip_reason = "persist_failed";
		return result;
	}

	result.history = compacted;
	result.summary = llm_summary;
	result.compacted = true;
	result.skip_reason.clear();

	LOG_INFO("History compacted with LLM summary: tokens=" + std::to_string(total_tokens) +
			 ", keep_recent_turns=" + std::to_string(cfg.keep_recent_turns) +
			 ", new_turns=" + std::to_string(result.history.size()));

	return result;
}

} // namespace velix::llm
