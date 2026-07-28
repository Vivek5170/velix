#pragma once

#include "../communication/json_include.hpp"
#include <string>

namespace velix::llm {

inline constexpr int kCompacterInternalPid = 2147483647;

struct CompactResult {
    nlohmann::json history;
    bool compacted = false;
    std::string summary;
    std::string skip_reason;
};

/**
 * Compact long history by summarizing older turns via Scheduler/LLM.
 *
 * Behavior is driven by config/compacter.json, including keep_recent_turns
 * and summarization sampling parameters.
 */
CompactResult compact_history_if_needed(const nlohmann::json& history);

} // namespace velix::llm
