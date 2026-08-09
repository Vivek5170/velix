#include "scheduler.hpp"
#include "compacter.hpp"
#include "session_io.hpp"
#include "session_manager.hpp"
#include "storage/provider_factory.hpp"
#include "tools/registry.hpp"

#include "../communication/asio_framing.hpp"
#include "../communication/network_config.hpp"
#include "../communication/socket_wrapper.hpp"
#include "../utils/asio_runtime.hpp"
#include "../utils/config_utils.hpp"
#include "../utils/logger.hpp"
#include "../utils/string_utils.hpp"
#include "../utils/thread_pool.hpp"
#include "../utils/timer.hpp"
#include "../communication/json_include.hpp"

#include <asio.hpp>


#include "adapters/factory.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using json = nlohmann::json;

namespace velix::llm {

namespace {

namespace fs = std::filesystem;

bool load_json_with_fallback(const std::vector<std::string> &paths, json &out);

struct ModelConfig {
  std::string model_name{"unknown"};
  std::string model_type{"unknown"};
  std::string active_adapter{"unknown"};
  std::size_t context_length{0};
  int max_simultaneous_llm_requests{0};
  bool enabled{true};
};

struct SchedulerConfig {
  std::string active_adapter;
  adapters::AdapterConfig adapter_cfg;
  ModelConfig model_info;
  double auto_compact_threshold{0.70};
  int max_llm_keys{5};
  int max_client_threads{64}; // Tier 1: Lobby Pool Size
  int scheduler_wait_timeout_ms{65000};
  int executioner_port{5172};
  int supervisor_port{5173};
};

struct PendingRequest {
  std::string request_id;
  std::string trace_id;  // for client cancellation tracking
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

struct ActiveRequest {
  std::string request_id; // unique per attempt for a trace
};

std::string load_scheduler_session_io_root() {
  json cfg;
  if (load_json_with_fallback({"config/storage.json", "../config/storage.json",
                               "build/config/storage.json"},
                              cfg)) {
    return cfg.value("json_root", std::string("memory/sessions"));
  }
  return "memory/sessions";
}

std::string session_manager_root_from_session_io_root(
    const std::string &session_io_root) {
  fs::path root(session_io_root);
  if (root.filename() == "sessions" && root.has_parent_path()) {
    return root.parent_path().string();
  }
  return "memory";
}

class SchedulerService {
 public:
  SchedulerService()
      : storage_provider(storage::make_storage_provider_from_config()),
        session_io(storage_provider, load_scheduler_session_io_root()),
        session_manager(
            session_manager_root_from_session_io_root(load_scheduler_session_io_root()),
            storage_provider) {}

  void shutdown() {
    shutting_down.store(true);
    queue_cv.notify_all();
  }

  std::mutex queue_mutex;
  std::condition_variable queue_cv;
  std::unordered_map<std::string, TreeQueue> tree_queues;
  std::priority_queue<TreeCandidate, std::vector<TreeCandidate>,
                      TreeCandidateCompare>
      ready_tree_queue;
  std::atomic<bool> shutting_down{false};
  std::shared_ptr<storage::IStorageProvider> storage_provider;
  SessionIO session_io;
  SessionManager session_manager;
  tools::ToolRegistry tool_registry;
  std::mutex trace_mutex;
  std::unordered_map<std::string, ActiveRequest> active_requests;
  // Task 14: Per-tree LLM request counts tracked locally to eliminate the
  // N+1 TCP round trip to the Supervisor that previously happened on every
  // LLM request. Protected by queue_mutex (already held by worker_loop).
  std::unordered_map<std::string, int> llm_request_counts;
};

SchedulerService &scheduler_service() {
  static SchedulerService svc;
  return svc;
}

#define queue_mutex (scheduler_service().queue_mutex)
#define queue_cv (scheduler_service().queue_cv)
#define tree_queues (scheduler_service().tree_queues)
#define ready_tree_queue (scheduler_service().ready_tree_queue)
#define shutting_down (scheduler_service().shutting_down)
#define storage_provider (scheduler_service().storage_provider)
#define session_io (scheduler_service().session_io)
#define session_manager (scheduler_service().session_manager)
#define tool_registry (scheduler_service().tool_registry)
#define trace_mutex (scheduler_service().trace_mutex)
#define active_requests (scheduler_service().active_requests)
#define llm_request_counts (scheduler_service().llm_request_counts)

void mark_request_active(const std::string &trace_id,
                         const std::string &request_id) {
  if (trace_id.empty() || request_id.empty()) {
    return;
  }

  std::scoped_lock<std::mutex> lock(trace_mutex);
  active_requests[trace_id] = ActiveRequest{request_id};
}

bool is_request_current(const std::string &trace_id,
                        const std::string &request_id) {
  if (trace_id.empty() || request_id.empty()) {
    return true;
  }

  std::scoped_lock<std::mutex> lock(trace_mutex);
  auto it = active_requests.find(trace_id);
  return it != active_requests.end() && it->second.request_id == request_id;
}

void clear_request_if_current(const std::string &trace_id,
                              const std::string &request_id) {
  if (trace_id.empty() || request_id.empty()) {
    return;
  }

  std::scoped_lock<std::mutex> lock(trace_mutex);
  auto it = active_requests.find(trace_id);
  if (it != active_requests.end() && it->second.request_id == request_id) {
    active_requests.erase(it);
  }
}

void safe_set_promise_value(const PendingRequest &req, const json &value) {
  if (!req.completion) {
    return;
  }

  try {
    req.completion->set_value(value);
  } catch (const std::future_error &e) {
    LOG_DEBUG("Skipping completion for request_id=" + req.request_id +
              ": future not waiting (" + std::string(e.what()) + ")");
  }
}

using SignalHandler = void (*)(int);
SignalHandler previous_sigint_handler = SIG_DFL;
SignalHandler previous_sigterm_handler = SIG_DFL;

static std::mutex scheduler_lifecycle_mutex;
static std::condition_variable scheduler_lifecycle_cv;
static velix::utils::AsioRuntime scheduler_runtime;
static asio::ip::tcp::acceptor scheduler_acceptor{scheduler_runtime.context()};

void shutdown_scheduler() {
  shutting_down.store(true);
  queue_cv.notify_all();
  scheduler_lifecycle_cv.notify_all();
  asio::error_code ec;
  scheduler_acceptor.cancel(ec);
  scheduler_acceptor.close(ec);
}

void handle_shutdown_signal(int signum) {
  shutting_down.store(true);
  scheduler_lifecycle_cv.notify_all();

  SignalHandler previous =
      (signum == SIGINT) ? previous_sigint_handler : previous_sigterm_handler;
  if (previous && previous != SIG_DFL && previous != SIG_IGN &&
      previous != handle_shutdown_signal) {
    previous(signum);
  }
}

void handle_client_connection(velix::communication::SocketWrapper client_socket,
                              const SchedulerConfig &cfg);

void scheduler_accept_next(const SchedulerConfig &cfg,
                           velix::utils::ThreadPool &lobby_pool) {
  if (shutting_down.load()) {
    return;
  }
  scheduler_acceptor.async_accept(
      asio::make_strand(scheduler_runtime.context()),
      [&cfg, &lobby_pool](const asio::error_code &ec,
                          asio::ip::tcp::socket socket) {
        if (shutting_down.load()) {
          return;
        }
        if (ec) {
          if (ec != asio::error::operation_aborted) {
            LOG_WARN_CTX("Scheduler accept error: " + ec.message(), "scheduler",
                         "", -1, "accept_error");
          }
        } else {
          // Transfer ownership to a blocking SocketWrapper for the existing
          // handle_client_connection handler. Use dup on POSIX to keep the
          // asio socket's fd alive until the wrapper closes its copy.
          bool dispatched = false;
#if !defined(_WIN32) && !defined(_WIN64)
          int raw_fd = static_cast<int>(socket.native_handle());
          int dup_fd = ::dup(raw_fd);
          if (dup_fd != -1) {
            auto client_ptr =
                std::make_shared<velix::communication::SocketWrapper>();
            client_ptr->adopt_handle(dup_fd);
            dispatched = lobby_pool.try_submit(
                [client_ptr, cfg]() mutable {
                  handle_client_connection(std::move(*client_ptr), cfg);
                });
            if (!dispatched) {
              LOG_WARN(
                  "Scheduler lobby pool capacity reached; shedding load.");
              try {
                json err = {{"message_type", "LLM_RESPONSE"},
                            {"status", "error"},
                            {"error", "scheduler_capacity_reached"}};
                velix::communication::send_json(*client_ptr, err.dump());
              } catch (...) {
              }
            }
          } else {
            LOG_WARN_CTX("Scheduler dup failed: " +
                             std::string(strerror(errno)),
                         "scheduler", "", -1, "accept_error");
          }
#else
          // Windows: transfer ownership via release if available
          try {
            auto handle = socket.release();
            auto client_ptr =
                std::make_shared<velix::communication::SocketWrapper>();
            client_ptr->adopt_handle(handle);
            dispatched = lobby_pool.try_submit(
                [client_ptr, cfg]() mutable {
                  handle_client_connection(std::move(*client_ptr), cfg);
                });
            if (!dispatched) {
              LOG_WARN(
                  "Scheduler lobby pool capacity reached; shedding load.");
              try {
                json err = {{"message_type", "LLM_RESPONSE"},
                            {"status", "error"},
                            {"error", "scheduler_capacity_reached"}};
                velix::communication::send_json(*client_ptr, err.dump());
              } catch (...) {
              }
            }
          } catch (const std::exception &e) {
            LOG_WARN_CTX(std::string("Scheduler accept dispatch failed: ") +
                             e.what(),
                         "scheduler", "", -1, "accept_error");
          }
#endif
        }
        scheduler_accept_next(cfg, lobby_pool);
      });
}

bool load_json_with_fallback(const std::vector<std::string> &paths, json &out) {
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

std::string load_text_with_fallback(const std::vector<std::string> &paths) {
  for (const auto &path : paths) {
    std::ifstream in(path);
    if (!in.is_open()) {
      continue;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
  }
  return "";
}

SchedulerConfig load_scheduler_config() {
  SchedulerConfig cfg;

  {
    json model_json;
    if (!load_json_with_fallback({"config/model.json", "../config/model.json",
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

    // env precedence (explicit config -> adapter api_key_env name -> .env
    // fields -> process env)
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
    cfg.adapter_cfg.chat_endpoint =
        adapter.value("chat_completions_path", cfg.adapter_cfg.chat_endpoint);
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
    cfg.model_info.active_adapter = cfg.active_adapter;
    cfg.model_info.model_name =
      adapter.value("model", model_json.value("model_name", std::string("unknown")));
    cfg.model_info.model_type = adapter.value(
      "api_style", model_json.value("model_type", std::string("unknown")));
    cfg.model_info.enabled =
      adapter.value("enabled", model_json.value("enabled", true));
    cfg.model_info.max_simultaneous_llm_requests =
      model_json.value("max_simultaneous_llm_requests", 5);
    cfg.model_info.context_length =
      model_json.value("max_context_tokens", std::size_t{0});

    cfg.max_llm_keys = cfg.model_info.max_simultaneous_llm_requests;
    cfg.max_client_threads = model_json.value("max_client_threads", 64);
  }

    {
    json compacter_json;
    if (load_json_with_fallback({"config/compacter.json",
                   "../config/compacter.json",
                   "build/config/compacter.json"},
                  compacter_json) &&
      compacter_json.contains("auto_compact_threshold") &&
      compacter_json["auto_compact_threshold"].is_number()) {
      cfg.auto_compact_threshold =
        compacter_json["auto_compact_threshold"].get<double>();
    }
    }

  cfg.executioner_port = velix::utils::get_port("EXECUTIONER", 5172);
  cfg.supervisor_port = velix::utils::get_port("SUPERVISOR", 5173);

  if (cfg.max_llm_keys <= 0) {
    cfg.max_llm_keys = 1;
  }

  return cfg;
}

void attach_session_alias_fields(json &reply) {
  if (!reply.contains("session") || !reply["session"].is_object()) {
    return;
  }

  const json &session = reply["session"];
  reply["session_id"] = session.value("session_id", std::string(""));
  reply["title"] = session.value("title", std::string(""));
}

void handle_session_control(const json &envelope,
                            velix::communication::SocketWrapper &socket,
                            SessionManager &sm, SessionIO &session_io_ref,
                            const SchedulerConfig &cfg) {
  const std::string action = envelope.value("action", std::string(""));
  const std::string user_id = envelope.value("user_id", std::string(""));
  const std::string title = envelope.value("title", std::string(""));

  json reply = {{"message_type", "SESSION_RESPONSE"}, {"action", action}};

  try {
    const ModelConfig &mc = cfg.model_info;

    const auto target = SessionManager::resolve_target(user_id);
    const std::string super_user = target.super_user;
    const std::string session_id = target.session_id;

    if (action == "get_or_create") {
      if (super_user.empty()) {
        throw std::runtime_error("'get_or_create' requires user_id");
      }

      const std::string sid = sm.get_or_create_active_session(super_user);
      reply["session"] = sm.get_session_object(sid, mc.context_length);
      attach_session_alias_fields(reply);
    } else if (action == "new") {
      if (super_user.empty()) {
        throw std::runtime_error("'new' requires user_id");
      }

      const std::string sid = sm.new_session(super_user, title);
      reply["session"] = sm.get_session_object(sid, mc.context_length);
      attach_session_alias_fields(reply);
    } else if (action == "create_super_user") {
      const std::string name =
          (envelope.contains("super_user") && envelope["super_user"].is_string() &&
           !envelope["super_user"].get<std::string>().empty())
              ? envelope["super_user"].get<std::string>()
              : user_id;

      sm.create_super_user(name);
      reply["super_user"] = name;
    } else if (action == "set_title") {
      if (session_id.empty()) {
        throw std::runtime_error(
            "'set_title' requires a full session_id (e.g. vivek_s2)");
      }

      sm.set_session_title(session_id, title);
      reply["session"] = sm.get_session_object(session_id, mc.context_length);
      attach_session_alias_fields(reply);
    } else if (action == "delete") {
      if (session_id.empty()) {
        throw std::runtime_error(
            "'delete' requires a full session_id (e.g. vivek_s2)");
      }

      const bool deleted = sm.delete_session(session_id);
      session_io_ref.delete_conversation(session_id, -1);
      session_io_ref.invalidate_conversation_cache(session_id);

      reply["deleted"] = deleted;
      reply["session_id"] = session_id;
      reply["super_user"] = super_user;
    } else if (action == "destroy_user") {
      if (super_user.empty()) {
        throw std::runtime_error("'destroy_user' requires user_id");
      }

      const auto sessions = sm.list_sessions(super_user);
      for (const auto &sid : sessions) {
        session_io_ref.invalidate_conversation_cache(sid);
      }

      const bool deleted = sm.delete_super_user(super_user);
      reply["destroyed"] = deleted;
      reply["super_user"] = super_user;
     } else if (action == "compact") {
       const std::string resolved_target =
           !session_id.empty() ? session_id
                               : sm.get_or_create_active_session(super_user);

       const Conversation convo = session_io_ref.get_conversation(resolved_target);
       json history = json::array();
       for (const auto &message : convo.messages) {
         if (message.is_object()) {
           history.push_back(message);
         }
       }

        const auto compact_result = sm.compact(resolved_target, history, false);
        if (compact_result.compacted) {
          // Persist the seeded conversation via storage provider
          if (!compact_result.compacted_conversation.empty()) {
            storage_provider->upsert_conversation(compact_result.compacted_conversation);
          }
          session_io_ref.invalidate_conversation_cache(resolved_target);
        }

       reply["session"] = sm.get_session_object(resolved_target, mc.context_length);
      reply["summary"] = compact_result.summary;
      reply["tokens_before"] = compact_result.tokens_before;
      reply["tokens_after"] = compact_result.tokens_after;
      reply["session_compacted"] = compact_result.compacted;
      reply["compact_reason"] = compact_result.compact_reason;
      attach_session_alias_fields(reply);
       } else if (action == "undo") {
         const std::string resolved_target =
             !session_id.empty() ? session_id
                                 : sm.get_or_create_active_session(super_user);

         Conversation convo = session_io_ref.get_conversation(resolved_target);
         auto &messages = convo.messages;

         int last_assistant = -1;
         for (int i = static_cast<int>(messages.size()) - 1; i >= 0; --i) {
           if (messages[i].is_object() &&
               messages[i].value("role", std::string("")) == "assistant") {
             last_assistant = i;
             break;
           }
         }

         int cut_from = -1;
         if (last_assistant >= 0) {
           for (int i = last_assistant - 1; i >= 0; --i) {
             if (!messages[i].is_object()) {
               continue;
             }
             if (messages[i].value("role", std::string("")) == "user") {
               cut_from = i;
               break;
             }
           }
         }

         int removed = 0;
         if (cut_from >= 0) {
           removed = static_cast<int>(messages.size()) - cut_from;
           messages.erase(messages.begin() + cut_from, messages.end());
           
           int new_turns = 0;
           uint64_t new_tokens = 0;
           for (const auto &m : messages) {
               if (!m.is_object()) continue;
               const std::string r = m.value("role", "");
               if (r == "user" || r == "assistant") new_turns++;
               const std::string c = m.value("content", "");
                if (!c.empty()) new_tokens += static_cast<uint64_t>(std::max<std::size_t>(1, c.size() / 3));
                if (m.contains("tool_calls") && m["tool_calls"].is_array()) {
                    new_tokens += static_cast<uint64_t>(std::max<std::size_t>(1, m["tool_calls"].dump().size() / 3));
               }
           }
           convo.turn_count = new_turns;
           convo.current_context_tokens = new_tokens;
           
           // Update last_activity_ms to record the undo action
           const auto now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch()).count();
           convo.last_activity_ms = now_ms;
           session_io_ref.persist_conversation(convo);
           // Invalidate cache so next retrieval gets the updated version from storage
           session_io_ref.invalidate_conversation_cache(resolved_target);
         }

        reply["session"] = sm.get_session_object(resolved_target, mc.context_length);
       reply["turns_removed"] = removed;
       reply["turns_remaining"] = static_cast<int>(messages.size());
       attach_session_alias_fields(reply);
    } else if (action == "list") {
      if (super_user.empty()) {
        throw std::runtime_error("'list' requires user_id");
      }

      const auto info = sm.get_super_user_info(super_user);
      json sessions = json::array();
      for (const auto &si : info.sessions) {
        sessions.push_back(
            SessionManager::build_session_object(si, mc.context_length));
      }

      reply["super_user"] = super_user;
      reply["sessions"] = sessions;
    } else if (action == "list_super_users") {
      const auto all_users = sm.list_super_users();
      json users = json::array();
      for (const auto &super_user_name : all_users) {
        const auto su_info = sm.get_super_user_info(super_user_name);
        json sessions = json::array();
        for (const auto &si : su_info.sessions) {
          sessions.push_back({{"session_id", si.session_id},
                              {"title", si.title},
                              {"turn_count", si.live_stats.turn_count}});
        }
        users.push_back({{"super_user", super_user_name},
                         {"session_count",
                          static_cast<int>(su_info.sessions.size())},
                         {"sessions", sessions}});
      }

      reply["super_users"] = users;
    } else {
      throw std::runtime_error("Unknown SESSION_CONTROL action: '" + action +
                               "'");
    }
  } catch (const std::exception &e) {
    reply["error"] = e.what();
    LOG_WARN(std::string("SESSION_CONTROL [") + action + "]: " + e.what());
  }

  velix::communication::send_json(socket, reply.dump());
}

void handle_session_query(const json &envelope,
                          velix::communication::SocketWrapper &socket,
                          SessionManager &sm,
                          const tools::ToolRegistry &registry,
                          const std::unordered_map<std::string, TreeQueue>
                              &tree_queues_ref,
              std::mutex &queue_mutex_ref,
              const SchedulerConfig &cfg) {
  const std::string query_type =
      envelope.value("query_type", std::string(""));
  const std::string user_id = envelope.value("user_id", std::string(""));

  json reply = {{"message_type", "SESSION_QUERY_RESPONSE"},
                {"query_type", query_type}};

  try {
    const ModelConfig &mc = cfg.model_info;
    const double threshold = cfg.auto_compact_threshold;

    const auto target = SessionManager::resolve_target(user_id);
    const std::string super_user = target.super_user;
    const std::string session_id = target.session_id;

     if (query_type == "info") {
       if (super_user.empty()) {
         throw std::runtime_error("'info' query requires user_id");
       }

       const std::string resolved_target =
           !session_id.empty() ? session_id
                               : sm.get_or_create_active_session(super_user);

       const auto info = sm.get_session_info(resolved_target);
       reply["session"] =
           SessionManager::build_session_object(info, mc.context_length);
       const std::string mode =
           (resolved_target.rfind("proc_", 0) == 0) ? "conversation"
                                           : "user_conversation";
       const auto usage = sm.compute_context_usage(
           resolved_target, json::array(), registry, mode, mc.context_length);

       reply["session_tokens"] = usage.session_tokens;
       reply["system_prompt_tokens"] = usage.system_prompt_tokens;
       reply["tool_schema_tokens"] = usage.tool_schema_tokens;
       reply["request_tokens"] = usage.request_tokens;
       reply["total_context_tokens"] = usage.total_context_tokens;
       reply["max_context_tokens"] = usage.max_context_tokens;
       reply["context_fill_pct"] = usage.context_fill_pct;
       reply["auto_compact_threshold_pct"] =
           static_cast<int>(threshold * 100);

       if (usage.max_context_tokens > 0) {
         const double fill =
             static_cast<double>(usage.total_context_tokens) /
             static_cast<double>(usage.max_context_tokens);
         reply["context_warning"] = (fill >= threshold);
       }
    } else if (query_type == "queue_depth") {
      const std::string queue_key =
          envelope.value("queue_key", std::string(""));
      int depth = 0;
      int total = 0;
      {
        std::scoped_lock<std::mutex> lock(queue_mutex_ref);
        for (const auto &[key, queue] : tree_queues_ref) {
          const int size = static_cast<int>(queue.requests.size());
          total += size;
          if (!queue_key.empty() && key == queue_key) {
            depth = size;
          }
        }
        if (queue_key.empty()) {
          depth = total;
        }
      }

      reply["queue_key"] = queue_key.empty() ? "(all)" : queue_key;
      reply["queue_depth"] = depth;
      reply["total_pending"] = total;
    } else if (query_type == "model_info") {
      reply["model_name"] = mc.model_name;
      reply["model_type"] = mc.model_type;
      reply["active_adapter"] = mc.active_adapter;
      reply["context_length"] = mc.context_length;
      reply["max_context_tokens"] = mc.context_length;
      reply["max_simultaneous_llm_requests"] =
          mc.max_simultaneous_llm_requests;
      reply["enabled"] = mc.enabled;
      reply["auto_compact_threshold_pct"] =
          static_cast<int>(threshold * 100);
    } else if (query_type == "tool_info") {
      const json schemas = registry.get_tool_schemas();
      const int tool_count = static_cast<int>(schemas.size());
      const int tool_context_tokens = static_cast<int>(std::max<std::size_t>(1, schemas.dump().size() / 3));

      json tools = json::array();
      for (const auto &schema : schemas) {
        if (!schema.is_object()) {
          continue;
        }
        const json function = schema.value("function", json::object());
        tools.push_back({{"name", function.value("name", std::string(""))},
                         {"description",
                          function.value("description", std::string(""))}});
      }

      reply["tool_count"] = tool_count;
      reply["tools"] = tools;
      reply["tool_context_tokens"] = tool_context_tokens;

      if (mc.context_length > 0) {
        reply["tool_context_pct"] =
            static_cast<double>(
                static_cast<int>(static_cast<double>(tool_context_tokens) /
                                 static_cast<double>(mc.context_length) *
                                 1000.0)) /
            10.0;
      }
    } else if (query_type == "all_sessions") {
      const auto all_users = sm.list_super_users();
      json users = json::array();
      for (const auto &super_user_name : all_users) {
        const auto su_info = sm.get_super_user_info(super_user_name);
        json sessions = json::array();
        for (const auto &si : su_info.sessions) {
          sessions.push_back(
              SessionManager::build_session_object(si, mc.context_length));
        }

        users.push_back({{"super_user", super_user_name},
                         {"session_count",
                          static_cast<int>(su_info.sessions.size())},
                         {"sessions", sessions}});
      }

      reply["super_users"] = users;
    } else {
      throw std::runtime_error("Unknown SESSION_QUERY query_type: '" +
                               query_type + "'");
    }
  } catch (const std::exception &e) {
    reply["error"] = e.what();
    LOG_WARN(std::string("SESSION_QUERY [") + query_type + "]: " + e.what());
  }

  velix::communication::send_json(socket, reply.dump());
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
      if (!message.contains("tool_calls") ||
          !message["tool_calls"].is_array()) {
        continue;
      }

      for (auto &tool_call : message["tool_calls"]) {
        if (!tool_call.is_object()) {
          continue;
        }
        if (!tool_call.contains("function") ||
            !tool_call["function"].is_object()) {
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
  request.sampling_params =
      req.payload.value("sampling_params", json::object());

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
  const bool has_user_id = req.payload.contains("user_id") &&
                           req.payload["user_id"].is_string() &&
                           !req.payload["user_id"].get<std::string>().empty();
  const bool stream_allowed = (mode == "user_conversation") &&
                              (req.tree_id == "TREE_HANDLER") && has_user_id &&
                              cfg.adapter_cfg.enable_streaming;

  request.stream = req.payload.value("stream", false) && stream_allowed;
  request.extra_body = req.payload.value("extra_body", json::object());
  return request;
}

adapters::ChatResponse
run_chat_once(const adapters::ProviderAdapter &adapter,
              const SchedulerConfig &cfg, const adapters::ChatRequest &request,
              const std::function<void(const std::string &)> &on_token) {
  if (!request.stream) {
    return adapter.call_chat(cfg.adapter_cfg, request);
  }

  adapters::ChatResponse aggregated;
  std::vector<json> partial_tool_calls;

  auto merge_object = [](json &target, const json &delta,
                         const auto &self_ref) -> void {
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

  auto find_slot_for_delta =
      [&partial_tool_calls](const json &delta) -> std::size_t {
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
        if (!chunk.delta_tool_call.is_null() &&
            chunk.delta_tool_call.is_object()) {
          const std::size_t slot = find_slot_for_delta(chunk.delta_tool_call);
          if (slot >= partial_tool_calls.size()) {
            partial_tool_calls.resize(slot + 1, json::object());
          }
          merge_object(partial_tool_calls[slot], chunk.delta_tool_call,
                       merge_object);
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
    if (!tool_call.contains("type") || !tool_call["type"].is_string() ||
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
  const std::string name =
      fn.value("name", tool_call.value("name", std::string("")));
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

  return json{
      {"id", id},
      {"type", "function"},
      {"function",
       {{"name", name},
        {"arguments", normalize_tool_arguments_object(raw_arguments)}}}};
}

// Task 14: notify_supervisor_llm_request() has been removed from the hot path.
// The Scheduler now tracks LLM request counts locally (see llm_request_counts
// in SchedulerService) and infers is_handler from tree_id == "TREE_HANDLER".
// This function is kept as a monitoring-only helper for future use cases that
// need to push a count update to the Supervisor on demand.
static json notify_supervisor_llm_request_monitoring(const PendingRequest &req,
                                                     const SchedulerConfig &cfg) {
  json event = {
      {"message_type", "LLM_REQUEST"}, {"request_id", req.request_id},
      {"tree_id", req.tree_id},        {"source_pid", req.source_pid},
      {"priority", req.base_priority}, {"mode", req.payload.value("mode", "")}};

  const std::string mode = req.payload.value("mode", "simple");
  if (mode == "conversation" || mode == "user_conversation") {
    event["convo_id"] = req.payload.value("convo_id", "");
    event["user_id"] = req.payload.value("user_id", "");
  }

  velix::communication::SocketWrapper socket;
  socket.create_tcp_socket();
  socket.connect(
      velix::communication::resolve_service_host("SUPERVISOR", "127.0.0.1"),
      static_cast<uint16_t>(cfg.supervisor_port));
  socket.set_timeout_ms(2000);
  velix::communication::send_json(socket, event.dump());

  const json supervisor_response = velix::communication::recv_json_parsed(socket);

  if (supervisor_response.value("status", "error") != "ok") {
    throw std::runtime_error(
        "supervisor rejected LLM_REQUEST: " +
        supervisor_response.value("error", std::string("unknown")));
  }

  return supervisor_response;
}

json process_llm_request_stateless(
    PendingRequest &req, const SchedulerConfig &cfg,
    const std::function<bool()> &is_attempt_current) {
  if (!req.payload.contains("messages") ||
      !req.payload["messages"].is_array()) {
    throw std::runtime_error("LLM_REQUEST missing messages[]");
  }

  uint64_t input_tokens = SessionManager::estimate_request_tokens(req.payload["messages"]);
  int current_max = req.payload.value("max_tokens", 
      req.payload.value("sampling_params", json::object()).value("max_tokens", 8192));
  
  if (cfg.model_info.context_length > 0) {
      if (input_tokens >= static_cast<uint64_t>(cfg.model_info.context_length > 100 ? cfg.model_info.context_length - 100 : 0)) {
          throw std::runtime_error("Context length exceeded (" + std::to_string(input_tokens) + " > " + std::to_string(cfg.model_info.context_length) + " tokens). Please use /undo to remove recent large outputs or /new to start a new session.");
      }
      int available = static_cast<int>(cfg.model_info.context_length) - static_cast<int>(input_tokens) - 100;
      if (available < 10) available = 10;
      if (current_max > available) {
          req.payload["max_tokens"] = available;
          if (req.payload.contains("sampling_params") && req.payload["sampling_params"].is_object()) {
              req.payload["sampling_params"]["max_tokens"] = available;
          }
      }
  }

  // The conversation manager already produced a fully layered system prompt
  // (guidelines → soul → caller system_message) inside
  // build_conversation_messages_safely and build_simple_mode_messages. We use
  // the messages as-is.
  const std::string mode = req.payload.value("mode", "simple");

  velix::utils::Timer timer;
  timer.start();

  auto adapter = adapters::make_adapter(cfg.active_adapter);
  const adapters::ChatRequest chat_request =
      build_chat_request(req, cfg, req.payload["messages"]);

  const adapters::ChatResponse final_response =
      run_chat_once(*adapter, cfg, chat_request, req.stream_token_callback);

  // A retry may have superseded this attempt while inference was running.
  // Discard the stale result and prevent any conversation writes.
  if (is_attempt_current && !is_attempt_current()) {
    return json{};
  }

  json normalized_tool_calls = json::array();
  if (final_response.tool_calls.is_array()) {
    int call_index = 0;
    for (const auto &tool_call : final_response.tool_calls) {
      normalized_tool_calls.push_back(
          normalize_tool_call(tool_call, call_index++));
    }
  }

    // Persist only assistant-output growth in session history. total_tokens
    // includes prompt tokens, which are already represented by prior turns.
    const uint64_t completion_tokens =
      final_response.usage.value("completion_tokens", static_cast<uint64_t>(0));
      const uint64_t completion_estimate =
        !final_response.content.empty()
          ? static_cast<uint64_t>(std::max<std::size_t>(1, final_response.content.size() / 3))
          : static_cast<uint64_t>(std::max<std::size_t>(1, normalized_tool_calls.dump().size() / 3));
    const uint64_t tokens_used =
      completion_tokens > 0
        ? completion_tokens
          : completion_estimate;

  if ((mode == "conversation" || mode == "user_conversation") &&
      !normalized_tool_calls.empty()) {
    if (!session_io.persist_assistant_tool_call(
            req.payload, final_response.content, normalized_tool_calls, tokens_used)) {
      LOG_WARN("Failed to persist assistant tool-call turn for convo_id=" +
               req.payload.value("convo_id", std::string("")));
    }
  }

  if ((mode == "conversation" || mode == "user_conversation") &&
      !final_response.content.empty() && normalized_tool_calls.empty()) {
    if (!session_io.persist_assistant_response(
            req.payload, final_response.content, tokens_used)) {
      LOG_WARN("Failed to persist assistant response for convo_id=" +
               req.payload.value("convo_id", std::string("")));
    }
  }

  timer.stop();
  json response = {{"message_type", "LLM_RESPONSE"},
                   {"status", "ok"},
                   {"request_id", req.request_id},
                   {"tree_id", req.tree_id},
                   {"mode", mode},
                   {"latency_ms", timer.elapsed_ms()}};

  if (mode == "conversation" || mode == "user_conversation") {
    response["convo_id"] = req.payload.value("convo_id", "");
    response["auto_compacted"] = req.payload.value("auto_compacted", false);
    response["compact_skip_reason"] =
        req.payload.value("compact_skip_reason", std::string(""));
    if (req.payload.value("session_compacted", false)) {
      response["session_compacted"] = true;
      response["tokens_before"] = req.payload.value("tokens_before", 0);
      response["tokens_after"] = req.payload.value("tokens_after", 0);
    }
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
  LOG_INFO_CTX("Scheduler worker started: key_slot=" + std::to_string(worker_id), "scheduler", "", -1, "worker_start");

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
    bool enqueue_background_compaction = false;
    std::string background_compaction_convo_id;
    SessionManager::ContextUsage background_compaction_usage;
    try {
      // Worker Check: Is the client still alive in the lobby?
      const bool client_alive =
          is_request_current(req.trace_id, req.request_id);

      if (!client_alive) {
        LOG_INFO("Skipping LLM inference for cancelled trace_id: " +
                 req.trace_id);
      } else {
        // Task 14: Instead of calling notify_supervisor_llm_request() which
        // opened a fresh TCP connection to the Supervisor on every LLM request,
        // we now:
        //   (1) Increment the per-tree LLM request count locally.
        //   (2) Infer is_handler directly from tree_id (no network round trip).
        // The Supervisor's handle_llm_request() remains available as a
        // monitoring-only endpoint but is no longer on the hot path.
        const bool is_handler_req = (req.tree_id == "TREE_HANDLER");
        {
          std::scoped_lock<std::mutex> count_lock(queue_mutex);
          llm_request_counts[req.tree_id]++;
        }
        req.payload["is_handler"] = is_handler_req;
        req.payload = session_io.normalize_llm_request(req.payload);
        const std::string mode =
            req.payload.value("mode", std::string("simple"));
        if (mode == "conversation" || mode == "user_conversation") {
          if (req.payload.value("owner_pid", -1) <= 0 && req.source_pid > 0) {
            req.payload["owner_pid"] = req.source_pid;
          }

          if (!req.payload.contains("messages") ||
              !req.payload["messages"].is_array()) {
            req.payload["messages"] =
                session_io.build_conversation_messages_safely(
                    req.payload);
          }

        }
        response = process_llm_request_stateless(req, cfg, [&req]() {
          return is_request_current(req.trace_id, req.request_id);
        });

        if ((mode == "conversation" || mode == "user_conversation") &&
            response.value("status", "error") == "ok") {
          const std::string convo_id =
              req.payload.value("convo_id", std::string(""));
          if (!convo_id.empty()) {
            background_compaction_usage = session_manager.compute_context_usage(
                convo_id, json::array(), tool_registry, mode,
                cfg.model_info.context_length);
            if (background_compaction_usage.max_context_tokens > 0) {
              const double auto_compact_threshold =
                  cfg.auto_compact_threshold > 0.0 ? cfg.auto_compact_threshold
                                                   : 0.70;
              const double fill_ratio =
                  static_cast<double>(
                      background_compaction_usage.total_context_tokens) /
                  static_cast<double>(
                      background_compaction_usage.max_context_tokens);
              if (fill_ratio >= auto_compact_threshold) {
                enqueue_background_compaction = true;
                background_compaction_convo_id = convo_id;
                response["needs_compaction"] = true;
                response["auto_compaction_queued"] = true;
              }
            }
          }
        }
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
      safe_set_promise_value(req, response);
    } else {
      // Client gone. Just unblock with a cancelled stub.
      safe_set_promise_value(req, {{"message_type", "LLM_RESPONSE"},
                                   {"status", "cancelled"},
                                   {"request_id", req.request_id},
                                   {"trace_id", req.trace_id}});
    }

    {
      std::scoped_lock<std::mutex> lock(queue_mutex);
      release_tree_key(req.queue_key);
    }
    queue_cv.notify_one();

    if (enqueue_background_compaction) {
      session_manager.enqueue_auto_compaction_if_needed(
          background_compaction_convo_id, background_compaction_usage,
          cfg.auto_compact_threshold, session_io);
    }
  }
}

PendingRequest parse_request_payload(const std::string &raw_payload) {
  return parse_request_payload(json::parse(raw_payload));
}

// Overload that accepts an already-parsed envelope JSON to avoid dump/parse
// roundtrips for callers that already have a json object.
PendingRequest parse_request_payload(const json &envelope) {
  if (envelope.value("message_type", "") != "LLM_REQUEST") {
    throw std::runtime_error("Scheduler only accepts message_type=LLM_REQUEST");
  }

  json request_json = envelope;
  if (envelope.contains("payload") && envelope["payload"].is_object()) {
    request_json = envelope["payload"];
    request_json["message_type"] = "LLM_REQUEST";

    // Preserve envelope metadata if payload omitted them.
    if (!request_json.contains("request_id") &&
        envelope.contains("request_id")) {
      request_json["request_id"] = envelope["request_id"];
    }
    if (!request_json.contains("trace_id") && envelope.contains("trace_id")) {
      request_json["trace_id"] = envelope["trace_id"];
    }
    if (!request_json.contains("tree_id") && envelope.contains("tree_id")) {
      request_json["tree_id"] = envelope["tree_id"];
    }
    if (!request_json.contains("source_pid") &&
        envelope.contains("source_pid")) {
      request_json["source_pid"] = envelope["source_pid"];
    }
    if (!request_json.contains("priority") && envelope.contains("priority")) {
      request_json["priority"] = envelope["priority"];
    }
    if (!request_json.contains("mode") && envelope.contains("mode")) {
      request_json["mode"] = envelope["mode"];
    }
  }

  if (!request_json.contains("request_id") ||
      !request_json["request_id"].is_string() ||
      request_json["request_id"].get<std::string>().empty()) {
    throw std::runtime_error(
        "LLM_REQUEST requires non-empty string request_id");
  }
  if (!request_json.contains("tree_id") ||
      !request_json["tree_id"].is_string() ||
      request_json["tree_id"].get<std::string>().empty()) {
    throw std::runtime_error("LLM_REQUEST requires non-empty string tree_id");
  }
  const int source_pid = request_json.value("source_pid", 0);
  if (!request_json.contains("source_pid") ||
      !request_json["source_pid"].is_number_integer() ||
      (source_pid <= 0 && source_pid != kCompacterInternalPid)) {
    throw std::runtime_error(
        "LLM_REQUEST requires positive integer source_pid");
  }
  if (!request_json.contains("mode") || !request_json["mode"].is_string()) {
    throw std::runtime_error("LLM_REQUEST requires mode");
  }

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
  // one human's slow generation from blocking all other human users,
  // TREE_HANDLER requests are parallelized using their unique `convo_id`.
  // However, autonomous background agents (like a Research Agent) must be
  // strictly limited to ONE concurrent LLM request per tree to prevent a single
  // agent from monopolizing all GPU slots. We enforce this by queueing them
  // strictly by `tree_id`.
  {
    const std::string mode = request_json.value("mode", "simple");
    const std::string user_id = request_json.value("user_id", "");

    if (req.tree_id == "TREE_HANDLER" && mode == "user_conversation" &&
        !user_id.empty()) {
      req.queue_key = "user_" + user_id;
    } else {
      req.queue_key = req.tree_id; // Strict 1-key-per-tree GPU Lock
    }
  }

  req.payload = std::move(request_json);

  const std::string mode = req.payload.value("mode", "simple");
  if (mode == "simple") {
    if (req.payload.value("convo_id", std::string("")).size() > 0 ||
        req.payload.value("user_id", std::string("")).size() > 0) {
      throw std::runtime_error(
          "simple mode requires empty convo_id and user_id");
    }
    const bool has_messages_array =
        req.payload.contains("messages") && req.payload["messages"].is_array();
    const bool has_user_message =
        req.payload.contains("user_message") &&
        req.payload["user_message"].is_string() &&
        !req.payload["user_message"].get<std::string>().empty();
    if (!has_messages_array && !has_user_message) {
      throw std::runtime_error(
          "simple mode requires messages[] or user_message");
    }
  } else if (mode == "conversation" || mode == "user_conversation") {
    const bool has_messages_array =
        req.payload.contains("messages") && req.payload["messages"].is_array();
    const bool has_alt_input = req.payload.contains("user_message") ||
                               req.payload.contains("system_message") ||
                               req.payload.contains("tool_result") ||
                               req.payload.contains("tool_message") ||
                               req.payload.contains("tool_messages");
    if (!has_messages_array && !has_alt_input) {
      throw std::runtime_error("conversation modes require messages[] or "
                               "user_message/system_message/tool_result");
    }

    if (mode == "conversation") {
      if (!req.payload.value("user_id", std::string("")).empty()) {
        throw std::runtime_error("conversation mode requires empty user_id");
      }
    } else {
      if (req.payload.value("user_id", std::string("")).empty()) {
        throw std::runtime_error("user_conversation mode requires user_id");
      }
    }
  } else {
    throw std::runtime_error("unsupported mode: " + mode);
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
  std::string current_request_id;
  try {
    const json envelope = velix::communication::recv_json_parsed(*client_socket_ptr);

    // SESSION_CONTROL / SESSION_QUERY / GET_LLM_COUNT: synchronous control-plane endpoints.
    {
      const std::string message_type = envelope.value("message_type", "");

      if (message_type == "SESSION_CONTROL") {
        handle_session_control(envelope, *client_socket_ptr, session_manager,
                               session_io, cfg);
        return;
      }
      if (message_type == "SESSION_QUERY") {
        handle_session_query(envelope, *client_socket_ptr, session_manager,
                             tool_registry, tree_queues, queue_mutex, cfg);
        return;
      }
      if (message_type == "GET_LLM_COUNT") {
        // Task 14 monitoring endpoint: returns per-tree LLM request counts
        // tracked locally by the Scheduler (no Supervisor round trip needed).
        const std::string tree_id = envelope.value("tree_id", std::string(""));
        json reply = {{"message_type", "LLM_COUNT_RESPONSE"},
                      {"status", "ok"}};
        {
          std::scoped_lock<std::mutex> lock(queue_mutex);
          if (tree_id.empty()) {
            json counts = json::object();
            for (const auto &[tid, count] : llm_request_counts) {
              counts[tid] = count;
            }
            reply["counts"] = counts;
          } else {
            auto it = llm_request_counts.find(tree_id);
            reply["tree_id"] = tree_id;
            reply["count"] = (it != llm_request_counts.end()) ? it->second : 0;
          }
        }
        velix::communication::send_json(*client_socket_ptr, reply.dump());
        return;
      }
      if (message_type == "RELOAD_TOOLS") {
        // Task 19: Reload tool manifests from disk so newly installed tools
        // are picked up without a full system restart.
        tool_registry.reload();
        json reply = {{"message_type", "RELOAD_TOOLS_RESPONSE"},
                      {"status", "ok"}};
        velix::communication::send_json(*client_socket_ptr, reply.dump());
        return;
      }
    }
    // ── LLM_REQUEST (normal path) ─────────────────────────────────────────────────
    // We already parsed the envelope above; use the json-aware parser variant
    // to avoid serializing and reparsing the envelope.
    PendingRequest req = parse_request_payload(envelope);
    current_trace = req.trace_id;
    current_request_id = req.request_id;

    req.stream_token_callback = [client_socket_ptr, client_send_mutex,
                                 request_id =
                                     req.request_id](const std::string &delta) {
      if (delta.empty()) {
        return;
      }
      try {
        const json stream_chunk = {{"message_type", "LLM_STREAM_CHUNK"},
                                   {"request_id", request_id},
                                   {"delta", delta}};
        std::scoped_lock<std::mutex> lock(*client_send_mutex);
        velix::communication::send_json(*client_socket_ptr,
                                        stream_chunk.dump());
      } catch (...) {
      }
    };

    mark_request_active(req.trace_id, req.request_id);

    std::future<json> future = req.completion->get_future();
    const std::string queue_key = req.queue_key;
    const std::string trace_id = req.trace_id;
    const std::string request_id = req.request_id;

    {
      std::scoped_lock<std::mutex> lock(queue_mutex);
      TreeQueue &queue = tree_queues[queue_key];
      queue.requests.push_back(std::move(req));
      ++queue.version;
      enqueue_tree_candidate_if_eligible(queue_key);
    }
    queue_cv.notify_one();

    if (future.wait_for(std::chrono::milliseconds(
            cfg.scheduler_wait_timeout_ms)) != std::future_status::ready) {
      throw std::runtime_error("scheduler_request_deadline_exceeded");
    }

    json response = future.get();
    {
      std::scoped_lock<std::mutex> lock(*client_send_mutex);
      velix::communication::send_json(*client_socket_ptr, response.dump());
    }

    clear_request_if_current(trace_id, request_id);

  } catch (const std::exception &e) {
    LOG_ERROR_CTX(std::string("Scheduler client handling error: ") + e.what(), "scheduler", "", -1, "client_error");
    clear_request_if_current(current_trace, current_request_id);
    try {
      const json error = {{"message_type", "LLM_RESPONSE"},
                          {"status", "error"},
                          {"error", e.what()}};
      std::scoped_lock<std::mutex> lock(*client_send_mutex);
      velix::communication::send_json(*client_socket_ptr, error.dump());
    } catch (...) {
    }
  }
}

} // namespace

void stop_scheduler() { shutdown_scheduler(); }

void start_scheduler(int port) {
  shutting_down.store(false);
  previous_sigterm_handler = std::signal(SIGTERM, handle_shutdown_signal);
  previous_sigint_handler = std::signal(SIGINT, handle_shutdown_signal);

  const SchedulerConfig cfg = load_scheduler_config();
  const std::string bind_host =
      velix::communication::resolve_bind_host("SCHEDULER", "127.0.0.1");

  LOG_INFO_CTX("Starting Scheduler on " + bind_host + ":" + std::to_string(port) +
           " with max_llm_keys=" + std::to_string(cfg.max_llm_keys), "scheduler", "", -1, "startup");

  // Task 19: Background thread that reloads tool manifests every 60 seconds
  // so newly installed tools are discovered without a full restart.
  std::thread tool_reloader([&] {
    while (!shutting_down.load()) {
      std::this_thread::sleep_for(std::chrono::seconds(60));
      if (shutting_down.load()) break;
      tool_registry.reload();
    }
  });
  tool_reloader.detach();

  velix::utils::ThreadPool lobby_pool(cfg.max_client_threads, 512);

  std::vector<std::thread> workers;
  workers.reserve(static_cast<std::size_t>(cfg.max_llm_keys));
  for (int i = 0; i < cfg.max_llm_keys; ++i) {
    workers.emplace_back([cfg, i] { worker_loop(cfg, i); });
  }

  // ASIO async acceptor replaces the 250ms has_data poll loop.
  try {
    if (scheduler_acceptor.is_open()) {
      asio::error_code ec;
      scheduler_acceptor.close(ec);
    }
    asio::ip::tcp::endpoint endpoint(
        asio::ip::make_address(bind_host),
        static_cast<unsigned short>(port));
    scheduler_acceptor.open(endpoint.protocol());
    scheduler_acceptor.set_option(asio::socket_base::reuse_address(true));
    scheduler_acceptor.bind(endpoint);
    scheduler_acceptor.listen(64);
  } catch (const std::exception &e) {
    LOG_ERROR_CTX("Scheduler failed to bind: " + std::string(e.what()),
                  "scheduler", "", -1, "startup_failure");
    return;
  }

  LOG_INFO_CTX("Scheduler listening on " + bind_host + ":" + std::to_string(port), "scheduler", "", -1, "listen");

  scheduler_runtime.start(4);
  scheduler_accept_next(cfg, lobby_pool);

  {
    std::unique_lock<std::mutex> lock(scheduler_lifecycle_mutex);
    scheduler_lifecycle_cv.wait(lock,
                                [] { return shutting_down.load(); });
  }

  {
    asio::error_code ec;
    scheduler_acceptor.cancel(ec);
    scheduler_acceptor.close(ec);
  }
  scheduler_runtime.stop();

  // Ensure queue workers wake up for shutdown.
  queue_cv.notify_all();

  for (auto &worker : workers) {
    if (worker.joinable()) {
      worker.join();
    }
  }

  std::signal(SIGTERM, previous_sigterm_handler);
  std::signal(SIGINT, previous_sigint_handler);
}

} // namespace velix::llm
