#pragma once

#include "../vendor/nlohmann/json.hpp"
#include <string>

namespace velix::llm {

struct CompactResult {
    nlohmann::json history;
    bool compacted = false;
    std::string summary;
};

/**
 * Compact long history by summarizing older turns via Scheduler/LLM.
 *
 * Behavior is driven by config/compacter.json, including token limits,
 * keep_recent_turns, and summarization sampling parameters.
 */
CompactResult compact_history_if_needed(const nlohmann::json& history);

/**
 * Compact history and persist to a specific file path instead of config default.
 * Useful for per-conversation histories.
 */
CompactResult compact_history_if_needed(const nlohmann::json& history, const std::string& history_file_override);

} // namespace velix::llm
