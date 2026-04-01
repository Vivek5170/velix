#include "scheduler.hpp"
#include "conversation_manager.hpp"

#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
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
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <sstream>
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
  int max_exec_iterations{8};
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

// Active Trace Set: Records trace_ids currently held by client lobby threads.
// If a client disconnects, the lobby thread removes the trace_id.
// The GPU worker checks this set before calling LLM.
std::mutex trace_mutex;
std::unordered_set<std::string> active_traces;

std::string trim(const std::string &text) {
  const auto is_space = [](char c) {
    return c == ' ' || c == '\n' || c == '\r' || c == '\t';
  };

  std::size_t begin = 0;
  while (begin < text.size() && is_space(text[begin])) {
    ++begin;
  }

  std::size_t end = text.size();
  while (end > begin && is_space(text[end - 1])) {
    --end;
  }

  return text.substr(begin, end - begin);
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
    cfg.adapter_cfg.host = adapter.value("host", std::string(""));
    cfg.adapter_cfg.port = adapter.value("port", cfg.adapter_cfg.port);
    cfg.adapter_cfg.use_https = adapter.value("use_https", false);
    cfg.adapter_cfg.base_path = adapter.value("base_path", std::string(""));
    cfg.adapter_cfg.chat_endpoint = adapter.value(
      "chat_completions_path", cfg.adapter_cfg.chat_endpoint);

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
    cfg.max_exec_iterations = model_json.value("max_exec_iterations", 8);
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
  if (cfg.max_exec_iterations <= 0) {
    cfg.max_exec_iterations = 1;
  }

  return cfg;
}

std::string call_llm(const json &messages, const json &sampling_params,
                     const SchedulerConfig &cfg) {
  auto adapter = adapters::make_adapter(cfg.active_adapter);
  return adapter->call_chat(cfg.adapter_cfg, messages, sampling_params);
}

struct ExecParseResult {
  bool has_exec{false};
  bool malformed{false};
  std::string error;
  std::vector<std::string> blocks;
  std::string text_without_exec;
};

ExecParseResult parse_exec_blocks(const std::string &text) {
  ExecParseResult result;

  std::istringstream input(text);
  std::ostringstream output;
  std::ostringstream current_block;
  std::string line;
  bool in_exec_block = false;

  while (std::getline(input, line)) {
    const std::string marker = trim(line);

    if (!in_exec_block && marker == "EXEC") {
      result.has_exec = true;
      in_exec_block = true;
      current_block.str("");
      current_block.clear();
      continue;
    }

    if (!in_exec_block && (marker == "EXEC_END" || marker == "END_EXEC")) {
      result.malformed = true;
      result.error = "EXEC_END/END_EXEC found without matching EXEC";
      return result;
    }

    if (in_exec_block) {
      if (marker == "EXEC_END" || marker == "END_EXEC") {
        const std::string block_text = trim(current_block.str());
        if (block_text.empty()) {
          result.malformed = true;
          result.error = "EXEC block cannot be empty";
          return result;
        }

        result.blocks.push_back(block_text);
        in_exec_block = false;
        current_block.str("");
        current_block.clear();
        continue;
      }

      if (marker == "EXEC") {
        result.malformed = true;
        result.error = "Nested EXEC block is not allowed";
        return result;
      }

      current_block << line << '\n';
      continue;
    }

    output << line << '\n';
  }

  if (in_exec_block) {
    result.malformed = true;
    result.error = "EXEC block missing END_EXEC/EXEC_END terminator";
    return result;
  }

  result.text_without_exec = trim(output.str());
  return result;
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

  const json sampling_params =
      req.payload.value("sampling_params", json::object());
  json messages = req.payload["messages"];
  const std::string mode = req.payload.value("mode", "simple");

  velix::utils::Timer timer;
  timer.start();

  const std::string llm_output = call_llm(messages, sampling_params, cfg);
  
  const ExecParseResult exec_parse = parse_exec_blocks(llm_output);
  if (exec_parse.malformed) {
    throw std::runtime_error("invalid EXEC block format: " + exec_parse.error);
  }

  if (mode == "conversation") {
    // We MUST persist the assistant's output into the conversation log whether
    // it was plaintext or an EXEC block, so the LLM has context for the next turn.
    if (!conversation_manager.persist_assistant_response(req.payload, llm_output)) {
      LOG_WARN("Failed to persist assistant response for convo_id=" +
               req.payload.value("convo_id", std::string("")));
    }
  }

  timer.stop();
  json response = {
      {"status", "ok"},           {"request_id", req.request_id},
      {"tree_id", req.tree_id},   {"mode", mode},
      {"latency_ms", timer.elapsed_ms()}};

  if (mode == "conversation") {
    response["convo_id"] = req.payload.value("convo_id", "");
  }

  if (exec_parse.has_exec) {
    if (mode != "conversation") {
      throw std::runtime_error(
          "EXEC blocks are only allowed in conversation mode");
    }
    response["exec_required"] = true;
    response["exec_blocks"] = exec_parse.blocks;
    response["response"] = "EXEC dispatched; yielding GPU lock to orchestrator SDK";
  } else {
    std::string final_answer = exec_parse.text_without_exec;
    if (final_answer.empty()) {
      final_answer = llm_output;
    }
    response["exec_required"] = false;
    response["response"] = final_answer;
  }

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
      req.completion->set_value({{"status", "cancelled"}, {"trace_id", req.trace_id}});
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
                               req.payload.contains("tool_message");
    if (!has_messages_array && !has_alt_input) {
      throw std::runtime_error(
          "conversation LLM_REQUEST requires messages[] or user_message/system_message/tool_result");
    }
  }

  return req;
}

void handle_client_connection(velix::communication::SocketWrapper client_socket,
                const SchedulerConfig &cfg) {
  std::string current_trace;
  try {
    const std::string raw_payload =
        velix::communication::recv_json(client_socket);
    PendingRequest req = parse_request_payload(raw_payload);
    current_trace = req.trace_id;

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
    velix::communication::send_json(client_socket, response.dump());
    
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
      const json error = {{"status", "error"}, {"error", e.what()}};
      velix::communication::send_json(client_socket, error.dump());
    } catch (...) {}
  }
}

} // namespace

void start_scheduler(int port) {
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

  while (true) {
    try {
      velix::communication::SocketWrapper client_socket = server_socket.accept();
      
      auto client_ptr = std::make_shared<velix::communication::SocketWrapper>(std::move(client_socket));
      bool submitted = lobby_pool.try_submit([client_ptr, cfg]() mutable {
        handle_client_connection(std::move(*client_ptr), cfg);
      });

      if (!submitted) {
        LOG_WARN("Scheduler lobby pool capacity reached; shedding load.");
        try {
          json error = {{"status", "error"}, {"error", "scheduler_capacity_reached"}};
          // Note: accessing *client_ptr is safe here as the lambda hasn't run yet or we own the only copy if it failed
          velix::communication::send_json(*client_ptr, error.dump());
        } catch (...) {}
      }
    } catch (const std::exception &e) {
      LOG_WARN(std::string("Scheduler accept error: ") + e.what());
    }
  }

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

} // namespace velix::llm
