#include "scheduler.hpp"
#include "conversation_manager.hpp"
#include "tools/registry.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/string_utils.hpp"
#include "../utils/timer.hpp"
#include "../utils/thread_pool.hpp"
#include "../vendor/nlohmann/json.hpp"

#define CPPHTTPLIB_IMPLEMENTATION
#include "../vendor/httplib.h"

#include "adapters/factory.hpp"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <csignal>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace velix::llm {

namespace {

struct SchedulerConfig {
  std::string active_adapter;
  adapters::AdapterConfig adapter_cfg;
  int max_llm_keys{5};
  int max_client_threads{64}; // Tier 1: Lobby Pool Size
  int scheduler_wait_timeout_ms{65000};
  int executioner_port{5172};
  int supervisor_port{5173};
};

struct PendingRequest {
  std::string request_id;
  std::string trace_id;   // for client cancellation tracking
  std::string tree_id;   // used only for supervisor notifications
  std::string queue_key; // serialization key: convo_id for conversation mode,
                         // tree_id for simple
  int source_pid{0};
  int base_priority{1};
  json payload;
  std::function<void(const std::string &)> stream_token_callback;
  std::chrono::steady_clock::time_point enqueued_at;
  std::shared_ptr<std::promise<json>> completion;
};

struct TreeQueue {
  std::deque<PendingRequest> requests;
  bool has_active_key{false};
  std::uint64_t version{0};
};

struct TreeCandidate {
  std::string queue_key; // convo_id or tree_id depending on mode
  double score{0.0};
  std::uint64_t version{0};
};

struct TreeCandidateCompare {
  bool operator()(const TreeCandidate &a, const TreeCandidate &b) const {
    return a.score < b.score;
  }
};

std::mutex queue_mutex;
std::condition_variable queue_cv;
std::unordered_map<std::string, TreeQueue> tree_queues;
std::priority_queue<TreeCandidate, std::vector<TreeCandidate>,
                    TreeCandidateCompare>
    ready_tree_queue;
std::atomic<bool> shutting_down{false};
ConversationManager conversation_manager;
tools::ToolRegistry tool_registry;

// Active Trace Set: Records trace_ids currently held by client lobby threads.
// If a client disconnects, the lobby thread removes the trace_id.
// The GPU worker checks this set before calling LLM.
std::mutex trace_mutex;
std::unordered_set<std::string> active_traces;

void shutdown_scheduler() {
  shutting_down.store(true);
  queue_cv.notify_all();
}

void handle_shutdown_signal(int) {
  shutting_down.store(true);
}

bool load_json_with_fallback(const std::vector<std::string> &paths,
                             json &out) {
  for (const auto &path : paths) {
    std::ifstream in(path);
    if (!in.is_open()) {
      continue;
    }
    try {
      in >> out;
      return true;
    } catch (...) {
      return false;
    }
  }
  return false;
}


SchedulerConfig load_scheduler_config() {
  SchedulerConfig cfg;

  {
    json model_json;
    if (!load_json_with_fallback(
            {"config/model.json", "../config/model.json",
             "build/config/model.json"},
            model_json)) {
      throw std::runtime_error("Missing config/model.json");
    }

    cfg.active_adapter = model_json.value("active_adapter", "llama.cpp");
    const json adapters = model_json.value("adapters", json::object());
    if (!adapters.contains(cfg.active_adapter) ||
        !adapters[cfg.active_adapter].is_object()) {
      throw std::runtime_error("Invalid active_adapter in config/model.json");
    }

    const json adapter = adapters[cfg.active_adapter];
    cfg.adapter_cfg.active_adapter = cfg.active_adapter;
    cfg.adapter_cfg.base_url =
      adapter.value("base_url", "http://127.0.0.1:8033/v1");
    cfg.adapter_cfg.model = adapter.value("model", "");

    // env precedence (explicit config -> adapter api_key_env name -> .env fields -> process env)
    std::string api_key = adapter.value("api_key", std::string(""));
    std::string env_var_name = adapter.value("api_key_env", std::string(""));

    auto dotenv_map = velix::utils::load_dotenv(".env");

    if (api_key.empty() && !env_var_name.empty()) {
      api_key = velix::utils::get_env_value(env_var_name, dotenv_map);
    }
    if (api_key.empty()) {
      api_key = velix::utils::get_env_value("OPENAI_API_KEY", dotenv_map);
    }
    if (api_key.empty()) {
      api_key = velix::utils::get_env_value("OLLAMA_API_KEY", dotenv_map);
    }
    cfg.adapter_cfg.api_key = api_key;

    cfg.adapter_cfg.host = adapter.value("host", std::string(""));
    cfg.adapter_cfg.port = adapter.value("port", cfg.adapter_cfg.port);
    cfg.adapter_cfg.use_https = adapter.value("use_https", false);
    cfg.adapter_cfg.base_path = adapter.value("base_path", std::string(""));
    cfg.adapter_cfg.chat_endpoint = adapter.value(
      "chat_completions_path", cfg.adapter_cfg.chat_endpoint);
    cfg.adapter_cfg.enable_tools = adapter.value("enable_tools", true);
    cfg.adapter_cfg.enable_streaming = adapter.value("enable_streaming", true);

    if (adapter.contains("stop_tokens") && adapter["stop_tokens"].is_array()) {
      for (const auto &token : adapter["stop_tokens"]) {
        if (token.is_string()) {
          cfg.adapter_cfg.stop_tokens.push_back(token.get<std::string>());
        }
      }
    }

    cfg.adapter_cfg.timeout_ms = model_json.value("request_timeout_ms", 60000);
    cfg.scheduler_wait_timeout_ms =
      model_json.value("request_timeout_ms", 60000) + 5000;
    const int configured_max_llm_keys = model_json.value("max_llm_keys", 5);
    const int configured_max_simultaneous = model_json.value(
        "max_simultaneous_llm_requests", configured_max_llm_keys);
    cfg.max_llm_keys = configured_max_simultaneous;
    cfg.max_client_threads = model_json.value("max_client_threads", 64);

    if (model_json.contains("max_llm_keys") &&
        model_json.contains("max_simultaneous_llm_requests") &&
        configured_max_llm_keys != configured_max_simultaneous) {
      LOG_WARN("Both max_llm_keys and max_simultaneous_llm_requests are set "
               "with different values; "
               "using max_simultaneous_llm_requests=" +
               std::to_string(configured_max_simultaneous));
    }
  }

  cfg.executioner_port = velix::utils::get_port("EXECUTIONER", 5172);
  cfg.supervisor_port = velix::utils::get_port("SUPERVISOR", 5173);

  if (cfg.max_llm_keys <= 0) {
    cfg.max_llm_keys = 1;
  }

  return cfg;
}

adapters::ChatRequest build_chat_request(const PendingRequest &req,
                                         const SchedulerConfig &cfg,
                                         const json &messages_override) {
  adapters::ChatRequest request;
  request.model = cfg.adapter_cfg.model;
  request.messages = messages_override;

  // Provider compatibility: assistant tool_calls.function.arguments are often
  // represented as JSON strings by OpenAI-compatible backends.
  if (request.messages.is_array()) {
    json system_messages = json::array();
    json non_system_messages = json::array();

    for (const auto &message : request.messages) {
      if (!message.is_object()) {
        continue;
      }
      const std::string role = message.value("role", std::string(""));
      if (role == "system") {
        system_messages.push_back(message);
      } else {
        non_system_messages.push_back(message);
      }
    }

    json normalized_messages = json::array();
    for (const auto &m : system_messages) {
      normalized_messages.push_back(m);
    }
    for (const auto &m : non_system_messages) {
      normalized_messages.push_back(m);
    }

    request.messages = std::move(normalized_messages);

    for (auto &message : request.messages) {
      if (!message.is_object()) {
        continue;
      }
      if (message.value("role", std::string("")) != "assistant") {
        continue;
      }
      if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) {
        continue;
      }

      for (auto &tool_call : message["tool_calls"]) {
        if (!tool_call.is_object()) {
          continue;
        }
        if (!tool_call.contains("function") || !tool_call["function"].is_object()) {
          continue;
        }
        json &fn = tool_call["function"];
        if (!fn.contains("arguments")) {
          continue;
        }
        if (fn["arguments"].is_object() || fn["arguments"].is_array()) {
          fn["arguments"] = fn["arguments"].dump();
        }
      }
    }
  }
  request.sampling_params = req.payload.value("sampling_params", json::object());

  request.max_tokens = req.payload.value(
      "max_tokens", request.sampling_params.value("max_tokens", 0));

  if (req.payload.contains("stop") && req.payload["stop"].is_array()) {
    for (const auto &token : req.payload["stop"]) {
      if (token.is_string()) {
        request.stop.push_back(token.get<std::string>());
      }
    }
  } else {
    request.stop = cfg.adapter_cfg.stop_tokens;
  }

  if (req.payload.contains("tools") && req.payload["tools"].is_array()) {
    request.tools = req.payload["tools"];
  } else if (cfg.adapter_cfg.enable_tools) {
    // Scheduler-owned default tool schema injection keeps SDKs portable.
    request.tools = tool_registry.get_tool_schemas();
  }

  if (req.payload.contains("tool_choice")) {
    request.tool_choice = req.payload["tool_choice"];
  } else if (request.tools.is_array() && !request.tools.empty()) {
    request.tool_choice = "auto";
  }

  const std::string mode = req.payload.value("mode", "simple");
  const std::string owner_type = req.payload.value("owner_type", "");
  const bool has_user_id = req.payload.contains("user_id") &&
                           req.payload["user_id"].is_string() &&
                           !req.payload["user_id"].get<std::string>().empty();
  const bool owner_stream_eligible =
      (owner_type == "process") || (owner_type == "user");
  const bool stream_allowed =
      (mode == "conversation") && (req.tree_id == "TREE_HANDLER") &&
      owner_stream_eligible && has_user_id &&
      cfg.adapter_cfg.enable_streaming;

  request.stream = req.payload.value("stream", false) && stream_allowed;
  request.extra_body = req.payload.value("extra_body", json::object());
  return request;
}

adapters::ChatResponse run_chat_once(const adapters::ProviderAdapter &adapter,
                                     const SchedulerConfig &cfg,
                                     const adapters::ChatRequest &request,
                                     const std::function<void(const std::string &)> &
                                         on_token) {
  if (!request.stream) {
    return adapter.call_chat(cfg.adapter_cfg, request);
  }

  adapters::ChatResponse aggregated;
  std::vector<json> partial_tool_calls;

  auto merge_object = [](json &target, const json &delta, const auto &self_ref) -> void {
    if (!delta.is_object()) {
      return;
    }
    for (auto it = delta.begin(); it != delta.end(); ++it) {
      const std::string key = it.key();
      const json &value = it.value();

      if (!target.contains(key)) {
        target[key] = value;
        continue;
      }

      json &existing = target[key];
      if (existing.is_object() && value.is_object()) {
        self_ref(existing, value, self_ref);
        continue;
      }

      // Streaming providers often fragment function.arguments across deltas.
      if (key == "arguments" && existing.is_string() && value.is_string()) {
        existing = existing.get<std::string>() + value.get<std::string>();
        continue;
      }

      if (key == "arguments" && existing.is_null()) {
        existing = value;
        continue;
      }

      if (existing.is_string() && value.is_string()) {
        existing = value;
        continue;
      }

      existing = value;
    }
  };

  auto find_slot_for_delta = [&partial_tool_calls](const json &delta) -> std::size_t {
    if (delta.contains("index") && delta["index"].is_number_integer()) {
      const int idx = delta["index"].get<int>();
      if (idx >= 0) {
        return static_cast<std::size_t>(idx);
      }
    }

    const std::string id = delta.value("id", std::string(""));
    if (!id.empty()) {
      for (std::size_t i = 0; i < partial_tool_calls.size(); ++i) {
        if (partial_tool_calls[i].value("id", std::string("")) == id) {
          return i;
        }
      }
    }

    if (partial_tool_calls.empty()) {
      return 0;
    }
    return partial_tool_calls.size() - 1;
  };
  adapter.call_chat_stream(
      cfg.adapter_cfg, request,
      [&aggregated, &on_token, &partial_tool_calls, &find_slot_for_delta,
       &merge_object](const adapters::StreamChunk &chunk) {
        aggregated.content += chunk.delta_text;
        if (!chunk.delta_text.empty() && on_token) {
          on_token(chunk.delta_text);
        }
        if (!chunk.delta_tool_call.is_null() && chunk.delta_tool_call.is_object()) {
          const std::size_t slot = find_slot_for_delta(chunk.delta_tool_call);
          if (slot >= partial_tool_calls.size()) {
            partial_tool_calls.resize(slot + 1, json::object());
          }
          merge_object(partial_tool_calls[slot], chunk.delta_tool_call, merge_object);
        }
        if (chunk.finished) {
          if (aggregated.finish_reason.empty()) {
            aggregated.finish_reason = "stop";
          }
        }
      });

  aggregated.tool_calls = json::array();
  for (auto &tool_call : partial_tool_calls) {
    if (!tool_call.is_object() || tool_call.empty()) {
      continue;
    }
    if (tool_call.contains("index")) {
      tool_call.erase("index");
    }
    if (!tool_call.contains("type") ||
        !tool_call["type"].is_string() ||
        tool_call["type"].get<std::string>().empty()) {
      tool_call["type"] = "function";
    }
    aggregated.tool_calls.push_back(tool_call);
  }
  return aggregated;
}

json normalize_tool_arguments_object(const json &raw_arguments) {
  if (raw_arguments.is_object()) {
    return raw_arguments;
  }

  if (raw_arguments.is_string()) {
    const std::string raw = raw_arguments.get<std::string>();
    if (raw.empty()) {
      return json::object();
    }

    try {
      const json parsed = json::parse(raw);
      if (parsed.is_object()) {
        return parsed;
      }
      return json{{"_raw", parsed.dump()}};
    } catch (...) {
      return json{{"_raw", raw}};
    }
  }

  if (raw_arguments.is_null()) {
    return json::object();
  }

  return json{{"_raw", raw_arguments.dump()}};
}

json normalize_tool_call(const json &tool_call, int fallback_index) {
  const json fn = tool_call.value("function", json::object());
  const std::string name = fn.value("name", tool_call.value("name", std::string("")));
  if (name.empty()) {
    throw std::runtime_error("tool_call missing function.name");
  }

  const json raw_arguments = fn.contains("arguments")
                                 ? fn["arguments"]
                                 : tool_call.value("arguments", json::object());

  std::string id = tool_call.value("id", std::string(""));
  if (id.empty()) {
    id = "call_" + std::to_string(fallback_index) + "_" +
         velix::utils::generate_uuid().substr(0, 8);
  }

  return json{{"id", id},
              {"type", "function"},
              {"function",
               {{"name", name},
                {"arguments", normalize_tool_arguments_object(raw_arguments)}}}};
}

json notify_supervisor_llm_request(const PendingRequest& req,
                                   const SchedulerConfig& cfg) {
  json event = {{"message_type", "LLM_REQUEST"},
                {"request_id",  req.request_id},
                {"tree_id",     req.tree_id},
                {"source_pid",  req.source_pid},
                {"priority",    req.base_priority}};

  const std::string mode = req.payload.value("mode", "simple");
  if (mode == "conversation") {
    event["convo_id"]   = req.payload.value("convo_id",   "");
    event["convo_type"] = req.payload.value("convo_type", "process");
    event["user_id"]    = req.payload.value("user_id",    "");
  }

  velix::communication::SocketWrapper socket;
  socket.create_tcp_socket();
  socket.connect(velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1"),
                 static_cast<uint16_t>(cfg.supervisor_port));
  socket.set_timeout_ms(2000);
  velix::communication::send_json(socket, event.dump());

  const std::string raw_response = velix::communication::recv_json(socket);
  const json supervisor_response = json::parse(raw_response);

  if (supervisor_response.value("status", "error") != "ok") {
    throw std::runtime_error(
        "supervisor rejected LLM_REQUEST: " +
        supervisor_response.value("error", std::string("unknown")));
  }

  return supervisor_response;
}

json process_llm_request_stateless(PendingRequest &req,
                                     const SchedulerConfig &cfg) {
  if (!req.payload.contains("messages") ||
      !req.payload["messages"].is_array()) {
    throw std::runtime_error("LLM_REQUEST missing messages[]");
  }

  const std::string mode = req.payload.value("mode", "simple");

  velix::utils::Timer timer;
  timer.start();

  auto adapter = adapters::make_adapter(cfg.active_adapter);
  const adapters::ChatRequest chat_request =
      build_chat_request(req, cfg, req.payload["messages"]);
  const adapters::ChatResponse final_response =
      run_chat_once(*adapter, cfg, chat_request, req.stream_token_callback);

  json normalized_tool_calls = json::array();
  if (final_response.tool_calls.is_array()) {
    int call_index = 0;
    for (const auto &tool_call : final_response.tool_calls) {
      normalized_tool_calls.push_back(normalize_tool_call(tool_call, call_index++));
    }
  }

  if (mode == "conversation" && !normalized_tool_calls.empty()) {
    if (!conversation_manager.persist_assistant_tool_call(
            req.payload, final_response.content, normalized_tool_calls)) {
      LOG_WARN("Failed to persist assistant tool-call turn for convo_id=" +
               req.payload.value("convo_id", std::string("")));
    }
  }

  if (mode == "conversation" && !final_response.content.empty() &&
      normalized_tool_calls.empty()) {
    if (!conversation_manager.persist_assistant_response(req.payload,
                                                         final_response.content)) {
      LOG_WARN("Failed to persist assistant response for convo_id=" +
               req.payload.value("convo_id", std::string("")));
    }
  }

  timer.stop();
  json response = {
      {"message_type", "LLM_RESPONSE"},
      {"status", "ok"},           {"request_id", req.request_id},
      {"tree_id", req.tree_id},   {"mode", mode},
      {"latency_ms", timer.elapsed_ms()}};

  if (mode == "conversation") {
    response["convo_id"] = req.payload.value("convo_id", "");
  }

  response["response"] = final_response.content;
  json assistant_message = {{"role", "assistant"},
                            {"tool_calls", normalized_tool_calls}};
  if (!final_response.content.empty()) {
    assistant_message["content"] = final_response.content;
  }
  response["assistant_message"] = assistant_message;
  response["tool_calls"] = normalized_tool_calls;
  response["finish_reason"] = final_response.finish_reason;
  response["usage"] = final_response.usage;
  response["raw_provider_response"] = final_response.raw;

  return response;
}

double effective_tree_priority(const PendingRequest &req) {
  const auto wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - req.enqueued_at)
                           .count();

  // Priority grows with wait time so older requests gain fairness while
  // preserving the caller's explicit base priority.
  return static_cast<double>(req.base_priority) +
         (static_cast<double>(wait_ms) / 5000.0);
}

void enqueue_tree_candidate_if_eligible(const std::string &queue_key) {
  auto it = tree_queues.find(queue_key);
  if (it == tree_queues.end()) {
    return;
  }

  TreeQueue &queue = it->second;
  if (queue.has_active_key || queue.requests.empty()) {
    return;
  }

  const double score = effective_tree_priority(queue.requests.front());
  ready_tree_queue.push(TreeCandidate{queue_key, score, queue.version});
  queue_cv.notify_one();
}

bool pick_next_request(PendingRequest &out) {
  while (!ready_tree_queue.empty()) {
    const TreeCandidate candidate = ready_tree_queue.top();
    ready_tree_queue.pop();

    auto it = tree_queues.find(candidate.queue_key);
    if (it == tree_queues.end()) {
      continue;
    }

    TreeQueue &queue = it->second;
    if (queue.version != candidate.version) {
      continue;
    }
    if (queue.has_active_key || queue.requests.empty()) {
      continue;
    }

    out = std::move(queue.requests.front());
    queue.requests.pop_front();
    queue.has_active_key = true;
    ++queue.version;
    return true;
  }

  return false;
}

void release_tree_key(const std::string &queue_key) {
  auto it = tree_queues.find(queue_key);
  if (it == tree_queues.end()) {
    return;
  }

  it->second.has_active_key = false;
  ++it->second.version;
  if (it->second.requests.empty()) {
    tree_queues.erase(it);
    return;
  }

  enqueue_tree_candidate_if_eligible(queue_key);
}

void worker_loop(const SchedulerConfig &cfg, int worker_id) {
  LOG_INFO("Scheduler worker started: key_slot=" + std::to_string(worker_id));

  while (true) {
    PendingRequest req;
    {
      std::unique_lock<std::mutex> lock(queue_mutex);
      queue_cv.wait(lock, [] {
        return !ready_tree_queue.empty() || shutting_down.load();
      });

      if (shutting_down.load()) {
        return;
      }

      if (!pick_next_request(req)) {
        continue;
      }
    }

    json response;
    try {
      // Worker Check: Is the client still alive in the lobby?
      bool client_alive = false;
      {
        std::lock_guard<std::mutex> lock(trace_mutex);
        client_alive = (active_traces.find(req.trace_id) != active_traces.end());
      }

      if (!client_alive) {
        LOG_INFO("Skipping LLM inference for cancelled trace_id: " + req.trace_id);
      } else {
        const json supervisor_response = notify_supervisor_llm_request(req, cfg);
        if (req.payload.value("mode", std::string("simple")) == "conversation") {
          const std::string canonical_owner_type =
              supervisor_response.value("owner_type", std::string(""));
          if (!canonical_owner_type.empty()) {
            req.payload["owner_type"] = canonical_owner_type;
          }

          if (req.payload.value("owner_pid", -1) <= 0 && req.source_pid > 0) {
            req.payload["owner_pid"] = req.source_pid;
          }

          if (!req.payload.contains("messages") || !req.payload["messages"].is_array()) {
            req.payload["messages"] =
                conversation_manager.build_conversation_messages_safely(req.payload);
          }
        }
        response = process_llm_request_stateless(req, cfg);
      }
    } catch (const std::exception &e) {
      response = {{"status", "error"},
                  {"message_type", "LLM_RESPONSE"},
                  {"request_id", req.request_id},
                  {"tree_id", req.tree_id},
                  {"error", e.what()}};
      LOG_ERROR(std::string("Scheduler failed request ") + req.request_id +
                ": " + e.what());
    }

    if (!response.empty()) {
      req.completion->set_value(response);
    } else {
      // Client gone. Just unblock with a cancelled stub.
      req.completion->set_value({{"message_type", "LLM_RESPONSE"},
                                 {"status", "cancelled"},
                                 {"trace_id", req.trace_id}});
    }

    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      release_tree_key(req.queue_key);
    }
    queue_cv.notify_one();
  }
}

PendingRequest parse_request_payload(const std::string &raw_payload) {
  const json envelope = json::parse(raw_payload);
  if (envelope.value("message_type", "") != "LLM_REQUEST") {
    throw std::runtime_error("Scheduler only accepts message_type=LLM_REQUEST");
  }

  json request_json = envelope;
  if (envelope.contains("payload") && envelope["payload"].is_object()) {
    request_json = envelope["payload"];
    request_json["message_type"] = "LLM_REQUEST";

    // Preserve envelope metadata if payload omitted them.
    if (!request_json.contains("request_id") && envelope.contains("request_id")) {
      request_json["request_id"] = envelope["request_id"];
    }
    if (!request_json.contains("trace_id") && envelope.contains("trace_id")) {
      request_json["trace_id"] = envelope["trace_id"];
    }
    if (!request_json.contains("tree_id") && envelope.contains("tree_id")) {
      request_json["tree_id"] = envelope["tree_id"];
    }
    if (!request_json.contains("source_pid") && envelope.contains("source_pid")) {
      request_json["source_pid"] = envelope["source_pid"];
    }
    if (!request_json.contains("priority") && envelope.contains("priority")) {
      request_json["priority"] = envelope["priority"];
    }
    if (!request_json.contains("mode") && envelope.contains("mode")) {
      request_json["mode"] = envelope["mode"];
    }
  }

  request_json = conversation_manager.normalize_llm_request(request_json);

  PendingRequest req;
  req.request_id = request_json.value("request_id", "");
  req.trace_id = request_json.value("trace_id", "");
  req.tree_id = request_json.value("tree_id", "");
  req.source_pid = request_json.value("source_pid", 0);
  req.base_priority = request_json.value("priority", 1);
  req.enqueued_at = std::chrono::steady_clock::now();
  req.completion = std::make_shared<std::promise<json>>();

  // Fix: Queue Limits vs Fairness
  // Human users via the Telegram handler reside in 'TREE_HANDLER'. To prevent 
  // one human's slow generation from blocking all other human users, TREE_HANDLER 
  // requests are parallelized using their unique `convo_id`. 
  // However, autonomous background agents (like a Research Agent) must be strictly 
  // limited to ONE concurrent LLM request per tree to prevent a single agent from 
  // monopolizing all GPU slots. We enforce this by queueing them strictly by `tree_id`.
  {
    const std::string mode = request_json.value("mode", "simple");
    const std::string convo_id = request_json.value("convo_id", "");
    
    if (req.tree_id == "TREE_HANDLER" && mode == "conversation" && !convo_id.empty()) {
      req.queue_key = convo_id; // Unmetered limit bypass for concurrent humans
    } else {
      req.queue_key = req.tree_id; // Strict 1-key-per-tree GPU Lock
    }
  }

  req.payload = std::move(request_json);

  if (req.request_id.empty() || req.tree_id.empty()) {
    throw std::runtime_error("LLM_REQUEST requires request_id and tree_id");
  }
  const std::string mode = req.payload.value("mode", "simple");
  if (mode == "simple") {
    if (!req.payload.contains("messages") ||
        !req.payload["messages"].is_array()) {
      throw std::runtime_error("LLM_REQUEST requires messages array");
    }
  } else if (mode == "conversation") {
    const bool has_messages_array =
        req.payload.contains("messages") && req.payload["messages"].is_array();
    const bool has_alt_input = req.payload.contains("user_message") ||
                               req.payload.contains("system_message") ||
                               req.payload.contains("tool_result") ||
                               req.payload.contains("tool_message") ||
                               req.payload.contains("tool_messages");
    if (!has_messages_array && !has_alt_input) {
      throw std::runtime_error(
          "conversation LLM_REQUEST requires messages[] or user_message/system_message/tool_result");
    }
  }

  return req;
}

void handle_client_connection(velix::communication::SocketWrapper client_socket,
                const SchedulerConfig &cfg) {
  auto client_socket_ptr =
      std::make_shared<velix::communication::SocketWrapper>(
          std::move(client_socket));
  auto client_send_mutex = std::make_shared<std::mutex>();

  std::string current_trace;
  try {
    const std::string raw_payload =
        velix::communication::recv_json(*client_socket_ptr);
    PendingRequest req = parse_request_payload(raw_payload);
    current_trace = req.trace_id;

    req.stream_token_callback =
        [client_socket_ptr, client_send_mutex,
         request_id = req.request_id](const std::string &delta) {
          if (delta.empty()) {
            return;
          }
          try {
            const json stream_chunk = {{"message_type", "LLM_STREAM_CHUNK"},
                                       {"request_id", request_id},
                                       {"delta", delta}};
            std::lock_guard<std::mutex> lock(*client_send_mutex);
            velix::communication::send_json(*client_socket_ptr,
                                            stream_chunk.dump());
          } catch (...) {
          }
        };

    // Register active trace for cancellation tracking
    {
      std::lock_guard<std::mutex> lock(trace_mutex);
      active_traces.insert(current_trace);
    }

    std::future<json> future = req.completion->get_future();
    const std::string queue_key = req.queue_key;

    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      TreeQueue &queue = tree_queues[queue_key];
      queue.requests.push_back(std::move(req));
      ++queue.version;
      enqueue_tree_candidate_if_eligible(queue_key);
    }
    queue_cv.notify_one();

    if (future.wait_for(
            std::chrono::milliseconds(cfg.scheduler_wait_timeout_ms)) !=
        std::future_status::ready) {
      throw std::runtime_error("scheduler_request_deadline_exceeded");
    }

    const json response = future.get();
    {
      std::lock_guard<std::mutex> lock(*client_send_mutex);
      velix::communication::send_json(*client_socket_ptr, response.dump());
    }
    
    // Cleanup trace
    {
      std::lock_guard<std::mutex> lock(trace_mutex);
      active_traces.erase(current_trace);
    }

  } catch (const std::exception &e) {
    LOG_ERROR(std::string("Scheduler client handling error: ") + e.what());
    if (!current_trace.empty()) {
      std::lock_guard<std::mutex> lock(trace_mutex);
      active_traces.erase(current_trace);
    }
    try {
      const json error = {{"message_type", "LLM_RESPONSE"},
                          {"status", "error"},
                          {"error", e.what()}};
      std::lock_guard<std::mutex> lock(*client_send_mutex);
      velix::communication::send_json(*client_socket_ptr, error.dump());
    } catch (...) {}
  }
}

} // namespace

void start_scheduler(int port) {
  shutting_down.store(false);
  std::signal(SIGTERM, handle_shutdown_signal);
  std::signal(SIGINT, handle_shutdown_signal);

  const SchedulerConfig cfg = load_scheduler_config();
  const std::string bind_host =
      velix::communication::resolve_bind_host("SCHEDULER", "127.0.0.1");

  LOG_INFO("Starting Scheduler on " + bind_host + ":" + std::to_string(port) +
           " with max_llm_keys=" + std::to_string(cfg.max_llm_keys));

  velix::utils::ThreadPool lobby_pool(cfg.max_client_threads, 512);

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(cfg.max_llm_keys));
  for (int i = 0; i < cfg.max_llm_keys; ++i) {
    workers.emplace_back([cfg, i] { worker_loop(cfg, i); });
  }

  velix::communication::SocketWrapper server_socket;
  server_socket.create_tcp_socket();
  server_socket.bind(bind_host, static_cast<uint16_t>(port));
  server_socket.listen(64);

  LOG_INFO("Scheduler listening on " + bind_host + ":" + std::to_string(port));

  while (!shutting_down.load()) {
    try {
      if (!server_socket.has_data(250)) {
        continue;
      }
      velix::communication::SocketWrapper client_socket = server_socket.accept();
      
      auto client_ptr = std::make_shared<velix::communication::SocketWrapper>(std::move(client_socket));
      bool submitted = lobby_pool.try_submit([client_ptr, cfg]() mutable {
        handle_client_connection(std::move(*client_ptr), cfg);
      });

      if (!submitted) {
        LOG_WARN("Scheduler lobby pool capacity reached; shedding load.");
        try {
          json error = {{"message_type", "LLM_RESPONSE"},
                        {"status", "error"},
                        {"error", "scheduler_capacity_reached"}};
          // Note: accessing *client_ptr is safe here as the lambda hasn't run yet or we own the only copy if it failed
          velix::communication::send_json(*client_ptr, error.dump());
        } catch (...) {}
      }
    } catch (const std::exception &e) {
      if (shutting_down.load()) {
        break;
      }
      LOG_WARN(std::string("Scheduler accept error: ") + e.what());
    }
  }

  shutdown_scheduler();

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

} // namespace velix::llm
