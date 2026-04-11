#include "compacter.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/logger.hpp"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <limits>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace velix::llm {

namespace {

struct CompacterConfig {
  bool enabled{true};
  std::size_t keep_recent_turns{16};
  std::size_t summary_max_tokens{300};
  double summary_temp{0.2};
  double summary_top_p{0.9};
  int scheduler_timeout_ms{30000};
  int summary_retry_count{2};
  int summary_retry_backoff_ms{250};
  std::string summary_tree_id{"TREE_HANDLER"};
  int summary_source_pid{1000};
};

bool is_transient_summary_error(const std::string &error_text) {
  return error_text.find("Recv timeout") != std::string::npos ||
         error_text.find("Resource temporarily unavailable") !=
             std::string::npos ||
         error_text.find("errno 11") != std::string::npos ||
         error_text.find("scheduler_request_deadline_exceeded") !=
             std::string::npos ||
         error_text.find("deadline_exceeded") != std::string::npos;
}

std::string classify_summary_skip_reason(const std::string &error_text) {
  if (is_transient_summary_error(error_text)) {
    return "summary_queue_timeout";
  }
  if (error_text.find("Scheduler summarization failed:") != std::string::npos) {
    return "summary_model_error";
  }
  return "summary_generation_failed";
}

int load_scheduler_wait_timeout_ms() {
  std::ifstream model_file("config/model.json");
  if (!model_file.is_open()) {
    model_file.open("../config/model.json");
  }
  if (!model_file.is_open()) {
    model_file.open("build/config/model.json");
  }
  if (!model_file.is_open()) {
    return 65000;
  }

  try {
    nlohmann::json model;
    model_file >> model;
    const int request_timeout_ms = model.value("request_timeout_ms", 60000);
    return request_timeout_ms + 5000;
  } catch (...) {
    return 65000;
  }
}

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

std::string random_id(const std::string &prefix) {
  static thread_local std::mt19937_64 rng{std::random_device{}()};
  const auto ticks =
      std::chrono::steady_clock::now().time_since_epoch().count();
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
    cfg.summary_max_tokens =
        j.value("summary_max_tokens", cfg.summary_max_tokens);
    cfg.summary_temp = j.value("summary_temp", cfg.summary_temp);
    cfg.summary_top_p = j.value("summary_top_p", cfg.summary_top_p);
    cfg.scheduler_timeout_ms =
        j.value("scheduler_timeout_ms", cfg.scheduler_timeout_ms);
    cfg.summary_retry_count =
        j.value("summary_retry_count", cfg.summary_retry_count);
    cfg.summary_retry_backoff_ms =
        j.value("summary_retry_backoff_ms", cfg.summary_retry_backoff_ms);
    cfg.summary_tree_id = j.value("summary_tree_id", cfg.summary_tree_id);
    cfg.summary_source_pid =
        j.value("summary_source_pid", cfg.summary_source_pid);
  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Failed parsing config/compacter.json: ") + e.what());
  }

  if (cfg.summary_source_pid <= 0) {
    cfg.summary_source_pid = 1000;
  }
  if (cfg.keep_recent_turns == 0) {
    cfg.keep_recent_turns = 1;
  }
  if (cfg.summary_retry_count < 0) {
    cfg.summary_retry_count = 0;
  }
  if (cfg.summary_retry_backoff_ms < 0) {
    cfg.summary_retry_backoff_ms = 0;
  }

  // Prevent compacter-side timeout from expiring before scheduler-side wait.
  const int scheduler_wait_timeout_ms = load_scheduler_wait_timeout_ms();
  const int aligned_timeout_ms = scheduler_wait_timeout_ms + 2000;
  if (cfg.scheduler_timeout_ms < aligned_timeout_ms) {
    LOG_INFO("Raising compacter scheduler_timeout_ms from " +
             std::to_string(cfg.scheduler_timeout_ms) + " to " +
             std::to_string(aligned_timeout_ms) +
             " to align with scheduler wait budget");
    cfg.scheduler_timeout_ms = aligned_timeout_ms;
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

std::size_t estimate_history_tokens(const nlohmann::json &history) {
  if (!history.is_array()) {
    return 0;
  }

  std::size_t total_chars = 0;
  for (const auto &turn : history) {
    if (!turn.is_object()) {
      continue;
    }
    if (turn.contains("content") && turn["content"].is_string()) {
      total_chars += turn["content"].get<std::string>().size();
    }
  }
  return total_chars / 4;
}

std::string
serialize_turns_for_summary_prompt(const nlohmann::json &older_turns) {
  std::ostringstream body;
  for (const auto &turn : older_turns) {
    if (!turn.is_object()) {
      continue;
    }

    const std::string role = turn.value("role", "unknown");
    const std::string content = turn.value("content", "");
    if (content.empty()) {
      continue;
    }

    body << '[';
    if (turn.contains("timestamp") && turn["timestamp"].is_string()) {
      body << turn["timestamp"].get<std::string>();
    } else {
      body << "unknown-time";
    }
    body << "] " << role << ": " << content << "\n";
  }
  return body.str();
}

const std::string SUMMARY_PREFIX =
    "[CONTEXT COMPACTION] Earlier turns in this conversation were compacted "
    "to save context space. The summary below describes work that was "
    "already completed, and the current session state may still reflect "
    "that work (for example, files may already be changed). Use the summary "
    "and the current state to continue from where things left off, and "
    "avoid repeating work:\n\n";

std::string request_llm_summary(const nlohmann::json &older_turns,
                                const CompacterConfig &cfg,
                                int scheduler_port) {
  std::string previous_summary;
  nlohmann::json turns_to_process = nlohmann::json::array();

  if (!older_turns.empty() && older_turns[0].is_object()) {
    const std::string first_content = older_turns[0].value("content", "");
    if (first_content.rfind(SUMMARY_PREFIX, 0) == 0) {
      previous_summary = first_content.substr(SUMMARY_PREFIX.size());
    }
  }

  for (std::size_t i = (previous_summary.empty() ? 0 : 1);
       i < older_turns.size(); ++i) {
    turns_to_process.push_back(older_turns[i]);
  }

  const std::string content_to_summarize =
      serialize_turns_for_summary_prompt(turns_to_process);
  std::ostringstream prompt;

  if (!previous_summary.empty()) {
    prompt
        << "Update the following technical handoff summary by incorporating "
           "the new conversation turns.\n\n"
        << "PREVIOUS SUMMARY:\n"
        << previous_summary << "\n\n"
        << "NEW TURNS TO INCORPORATE:\n"
        << content_to_summarize << "\n\n"
        << "Update the summary using this exact structure. PRESERVE all "
           "existing information that is still relevant. "
        << "ADD new progress. Move items from \"In Progress\" to \"Done\" when "
           "completed. Remove information only if it is clearly obsolete.\n\n"
        << "## Goal\n[What the user is trying to accomplish — preserve from "
           "previous summary, update if goal evolved]\n\n"
        << "## Constraints & Preferences\n[User preferences, coding style, "
           "constraints, important decisions — accumulate across "
           "compactions]\n\n"
        << "## Progress\n### Done\n[Completed work — include specific file "
           "paths, commands run, results obtained]\n"
        << "### In Progress\n[Work currently underway]\n"
        << "### Blocked\n[Any blockers or issues encountered]\n\n"
        << "## Key Decisions\n[Important technical decisions and why they were "
           "made]\n\n"
        << "## Relevant Files\n[Files read, modified, or created — with brief "
           "note on each. Accumulate across compactions.]\n\n"
        << "## Next Steps\n[What needs to happen next to continue the work]\n\n"
        << "## Critical Context\n[Any specific values, error messages, "
           "configuration details, or data that would be lost without explicit "
           "preservation]\n\n"
        << "Target ~" << cfg.summary_max_tokens
        << " tokens. Be specific — include file paths, command outputs, error "
           "messages, and concrete values rather than vague descriptions.\n\n"
        << "Write only the summary body. Do not include any preamble or "
           "prefix.";
  } else {
    prompt
        << "Create a structured technical handoff summary for a later "
           "assistant that will continue this conversation after earlier turns "
           "are compacted.\n\n"
        << "TURNS TO SUMMARIZE:\n"
        << content_to_summarize << "\n\n"
        << "Use this exact structure:\n\n"
        << "## Goal\n[What the user is trying to accomplish]\n\n"
        << "## Constraints & Preferences\n[User preferences, coding style, "
           "constraints, important decisions]\n\n"
        << "## Progress\n### Done\n[Completed work — include specific file "
           "paths, commands run, results obtained]\n"
        << "### In Progress\n[Work currently underway]\n"
        << "### Blocked\n[Any blockers or issues encountered]\n\n"
        << "## Key Decisions\n[Important technical decisions and why they were "
           "made]\n\n"
        << "## Relevant Files\n[Files read, modified, or created — with brief "
           "note on each]\n\n"
        << "## Next Steps\n[What needs to happen next to continue the work]\n\n"
        << "## Critical Context\n[Any specific values, error messages, "
           "configuration details, or data that would be lost without explicit "
           "preservation]\n\n"
        << "Target ~" << cfg.summary_max_tokens
        << " tokens. Be specific — include file paths, command outputs, error "
           "messages, and concrete values rather than vague descriptions. "
        << "The goal is to prevent the next assistant from repeating work or "
           "losing important details.\n\n"
        << "Write only the summary body. Do not include any preamble or "
           "prefix.";
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
      {"messages",
       nlohmann::json::array(
           {{{"role", "system"},
             {"content",
              "You are an expert technical summarizer that preserves project "
              "state across context compactions."}},
            {{"role", "user"}, {"content", prompt.str()}}})},
      {"sampling_params",
       {{"temp", cfg.summary_temp},
        {"top_p", cfg.summary_top_p},
        {"max_tokens", cfg.summary_max_tokens}}},
      {"metadata",
       {{"request_origin", "compacter"},
        {"compaction_request", true},
        {"timestamp", now_iso8601()}}}};

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
    throw std::runtime_error("Scheduler summarization failed: " +
                             response.value("error", "unknown"));
  }

  const std::string summary = response.value("response", "");
  if (summary.empty()) {
    throw std::runtime_error("Scheduler summarization returned empty summary");
  }

  return summary;
}

void copy_metadata_from_older(const nlohmann::json &older_turns,
                              nlohmann::json &summary_message) {
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

CompactResult compact_history_if_needed(const nlohmann::json &history) {
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
  if (history.size() <= cfg.keep_recent_turns + 1) {
    result.skip_reason = "below_preserve_limit";
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
  const int scheduler_port = load_scheduler_port();
  std::string last_error;
  const int attempts = cfg.summary_retry_count + 1;
  for (int attempt = 1; attempt <= attempts; ++attempt) {
    try {
      llm_summary = request_llm_summary(older, cfg, scheduler_port);
      last_error.clear();
      break;
    } catch (const std::exception &e) {
      last_error = e.what();
      const bool transient = is_transient_summary_error(last_error);
      LOG_WARN("Compaction summary attempt " + std::to_string(attempt) + "/" +
               std::to_string(attempts) + " failed: " + last_error +
               " [scheduler_port=" + std::to_string(scheduler_port) +
               ", timeout_ms=" + std::to_string(cfg.scheduler_timeout_ms) +
               ", older_turns=" + std::to_string(older.size()) +
               ", transient=" + std::string(transient ? "true" : "false") +
               "]");

      if (!transient || attempt == attempts) {
        break;
      }
      if (cfg.summary_retry_backoff_ms > 0) {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(cfg.summary_retry_backoff_ms * attempt));
      }
    }
  }

  if (llm_summary.empty()) {
    const std::string skip_reason = classify_summary_skip_reason(last_error);
    LOG_WARN(
        "Compaction skipped because summary generation failed after retries: " +
        last_error + " [skip_reason=" + skip_reason +
        ", possible causes: scheduler saturation, nested compaction "
        "contention, provider latency]");
    result.skip_reason = skip_reason;
    return result;
  }

  nlohmann::json compacted = nlohmann::json::array();
  nlohmann::json summary_message = {{"role", "assistant"},
                                    {"content", SUMMARY_PREFIX + llm_summary}};
  copy_metadata_from_older(older, summary_message);
  if (!summary_message.contains("timestamp")) {
    summary_message["timestamp"] = now_iso8601();
  }
  compacted.push_back(summary_message);

  for (const auto &turn : recent) {
    compacted.push_back(turn);
  }

  result.history = compacted;
  result.summary = llm_summary;
  result.compacted = true;
  result.skip_reason.clear();

  LOG_INFO("History compacted with LLM summary: tokens=" +
           std::to_string(total_tokens) +
           ", keep_recent_turns=" + std::to_string(cfg.keep_recent_turns) +
           ", new_turns=" + std::to_string(result.history.size()));

  return result;
}

} // namespace velix::llm
