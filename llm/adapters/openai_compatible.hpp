#pragma once

#include "provider.hpp"

#include "../../vendor/httplib.h"

#include <algorithm>
#include <functional>
#include <map>
#include <stdexcept>
#include <string>

namespace velix::llm::adapters {

struct SseEvent {
  std::string event;
  std::string data;
};

inline std::string ensure_leading_slash(const std::string &path) {
  if (path.empty()) {
    return "";
  }
  if (path.front() == '/') {
    return path;
  }
  return "/" + path;
}

inline std::string join_paths(const std::string &a, const std::string &b) {
  if (a.empty()) {
    return ensure_leading_slash(b);
  }
  if (b.empty()) {
    return ensure_leading_slash(a);
  }
  const std::string left = ensure_leading_slash(a);
  const std::string right = ensure_leading_slash(b);
  if (left.back() == '/' && right.front() == '/') {
    return left + right.substr(1);
  }
  return left + right;
}

inline void resolve_endpoint(const AdapterConfig &cfg, std::string &host,
                             int &port, bool &is_https,
                             std::string &base_path) {
  host = cfg.host;
  port = cfg.port;
  is_https = cfg.use_https;
  base_path = cfg.base_path;

  if (!host.empty()) {
    return;
  }

  std::string url = cfg.base_url;
  if (url.rfind("https://", 0) == 0) {
    is_https = true;
    url = url.substr(8);
  } else if (url.rfind("http://", 0) == 0) {
    is_https = false;
    url = url.substr(7);
  }

  std::string host_port = url;
  const std::size_t slash = url.find('/');
  if (slash != std::string::npos) {
    host_port = url.substr(0, slash);
    base_path = url.substr(slash);
  }

  host = host_port;
  port = is_https ? 443 : 80;
  const std::size_t colon = host_port.rfind(':');
  if (colon != std::string::npos) {
    host = host_port.substr(0, colon);
    try {
      port = std::stoi(host_port.substr(colon + 1));
    } catch (...) {
      throw std::runtime_error("Invalid adapter base_url port: " +
                               cfg.base_url);
    }
  }

  if (host.empty()) {
    throw std::runtime_error("LLM host is empty; check adapter host/base_url");
  }
}

inline std::string extract_error_message(const json &error_obj,
                                         const std::string &fallback_prefix) {
  if (error_obj.is_string()) {
    return fallback_prefix + ": " + error_obj.get<std::string>();
  }
  if (error_obj.is_object()) {
    if (error_obj.contains("message") && error_obj["message"].is_string()) {
      return fallback_prefix + ": " + error_obj["message"].get<std::string>();
    }
    return fallback_prefix + ": " + error_obj.dump();
  }
  return fallback_prefix;
}

inline std::string post_json(const AdapterConfig &cfg, const std::string &path,
                             const json &payload,
                             const httplib::Headers &extra_headers = {}) {
  std::string host;
  int port = cfg.port;
  bool is_https = cfg.use_https;
  std::string base_path;
  resolve_endpoint(cfg, host, port, is_https, base_path);

  const std::string endpoint = join_paths(base_path, path);
  httplib::Result res;

  const int timeout_ms = std::max(1, cfg.timeout_ms);
  const time_t timeout_sec = static_cast<time_t>(timeout_ms / 1000);
  const time_t timeout_usec = static_cast<time_t>((timeout_ms % 1000) * 1000);

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  if (is_https) {
    httplib::SSLClient cli(host, port);
    cli.set_connection_timeout(timeout_sec, timeout_usec);
    cli.set_read_timeout(timeout_sec, timeout_usec);
    cli.set_write_timeout(timeout_sec, timeout_usec);
    httplib::Headers headers;
    if (!cfg.api_key.empty()) {
      headers.emplace("Authorization", "Bearer " + cfg.api_key);
    }
    headers.insert(extra_headers.begin(), extra_headers.end());
    res = cli.Post(endpoint, headers, payload.dump(), "application/json");
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(timeout_sec, timeout_usec);
    cli.set_read_timeout(timeout_sec, timeout_usec);
    cli.set_write_timeout(timeout_sec, timeout_usec);
    httplib::Headers headers;
    if (!cfg.api_key.empty()) {
      headers.emplace("Authorization", "Bearer " + cfg.api_key);
    }
    headers.insert(extra_headers.begin(), extra_headers.end());
    res = cli.Post(endpoint, headers, payload.dump(), "application/json");
  }
#else
  if (is_https) {
    throw std::runtime_error(
        "HTTPS adapter endpoint configured but OpenSSL support is disabled");
  }
  httplib::Client cli(host, port);
  cli.set_connection_timeout(timeout_sec, timeout_usec);
  cli.set_read_timeout(timeout_sec, timeout_usec);
  cli.set_write_timeout(timeout_sec, timeout_usec);
  httplib::Headers headers;
  if (!cfg.api_key.empty()) {
    headers.emplace("Authorization", "Bearer " + cfg.api_key);
  }
  headers.insert(extra_headers.begin(), extra_headers.end());
  res = cli.Post(endpoint, headers, payload.dump(), "application/json");
#endif

  if (!res) {
    throw std::runtime_error("LLM request failed (Network / Timeout error)");
  }
  if (res->status != 200) {
    throw std::runtime_error("LLM request failed with status " +
                             std::to_string(res->status) + ": " + res->body);
  }

  return res->body;
}

inline void post_json_stream_raw(
    const AdapterConfig &cfg, const std::string &path, const json &payload,
    const std::function<bool(const char *, std::size_t)> &on_chunk,
    const httplib::Headers &extra_headers = {}) {
  std::string host;
  int port = cfg.port;
  bool is_https = cfg.use_https;
  std::string base_path;
  resolve_endpoint(cfg, host, port, is_https, base_path);

  const std::string endpoint = join_paths(base_path, path);
  httplib::Result res;

  const int timeout_ms = std::max(1, cfg.timeout_ms);
  const time_t timeout_sec = static_cast<time_t>(timeout_ms / 1000);
  const time_t timeout_usec = static_cast<time_t>((timeout_ms % 1000) * 1000);

  // Stream read timeout is intentionally decoupled from the connection timeout.
  // cfg.timeout_ms controls connection establishment and write.
  // cfg.stream_idle_timeout_ms controls max silence between received chunks.
  // Defaulting to 300s so slow local models (pre-first-token delay) don't get
  // killed by httplib before generation starts.
  const int stream_idle_ms =
      cfg.stream_idle_timeout_ms > 0 ? cfg.stream_idle_timeout_ms : 300000;
  const time_t idle_timeout_sec = static_cast<time_t>(stream_idle_ms / 1000);
  const time_t idle_timeout_usec =
      static_cast<time_t>((stream_idle_ms % 1000) * 1000);

  std::string dumped_payload = payload.dump();

  httplib::Headers headers;
  headers.emplace("Connection", "keep-alive");
  headers.emplace("Accept", "text/event-stream");
  if (!cfg.api_key.empty()) {
    headers.emplace("Authorization", "Bearer " + cfg.api_key);
  }
  headers.insert(extra_headers.begin(), extra_headers.end());

#ifdef CPPHTTPLIB_OPENSSL_SUPPORT
  if (is_https) {
    httplib::SSLClient cli(host, port);
    cli.set_connection_timeout(timeout_sec, timeout_usec);
    cli.set_read_timeout(idle_timeout_sec, idle_timeout_usec);
    cli.set_write_timeout(timeout_sec, timeout_usec);
    cli.set_keep_alive(true);
    res = cli.Post(endpoint, headers, dumped_payload, "application/json",
                   on_chunk);
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(timeout_sec, timeout_usec);
    cli.set_read_timeout(idle_timeout_sec, idle_timeout_usec);
    cli.set_write_timeout(timeout_sec, timeout_usec);
    cli.set_keep_alive(true);
    res = cli.Post(endpoint, headers, dumped_payload, "application/json",
                   on_chunk);
  }
#else
  if (is_https) {
    throw std::runtime_error(
        "HTTPS adapter endpoint configured but OpenSSL support is disabled");
  }
  httplib::Client cli(host, port);
  cli.set_connection_timeout(timeout_sec, timeout_usec);
  cli.set_read_timeout(idle_timeout_sec, idle_timeout_usec);
  cli.set_write_timeout(timeout_sec, timeout_usec);
  cli.set_keep_alive(true);
  res =
      cli.Post(endpoint, headers, dumped_payload, "application/json",
               [&](const char *data, std::size_t len) {
                 bool ok = on_chunk(data, len);
                 if (!ok) {
                   std::cerr
                       << "[Adapter] on_chunk returned false; stopping stream."
                       << std::endl;
                 }
                 return ok;
               });
#endif

  if (!res) {
    throw std::runtime_error(
        "LLM streaming request failed (Network / Timeout error)");
  }
  if (res->status != 200) {
    throw std::runtime_error("LLM streaming request failed with status " +
                             std::to_string(res->status) + ": " + res->body);
  }
}

inline void
stream_sse_events_ex(const AdapterConfig &cfg, const std::string &path,
                     const json &payload,
                     const std::function<bool(const SseEvent &)> &on_event,
                     const httplib::Headers &extra_headers = {}) {
  std::string line_buffer;
  std::size_t consumed = 0;
  std::string event_type;
  std::string event_data;
  std::string callback_error;
  bool should_continue = true;

  auto flush_event = [&on_event, &event_type, &event_data, &callback_error,
                      &should_continue]() {
    if (event_data.empty()) {
      return;
    }
    try {
      if (!on_event(SseEvent{event_type, event_data})) {
        should_continue = false;
      }
    } catch (const std::exception &e) {
      callback_error = e.what();
      should_continue = false;
    } catch (...) {
      callback_error = "Unknown SSE callback exception";
      should_continue = false;
    }
    event_type.clear();
    event_data.clear();
  };

  auto process_available_lines = [&line_buffer, &consumed, &event_type,
                                  &event_data, &flush_event,
                                  &should_continue]() {
    std::size_t newline_pos = std::string::npos;
    while (should_continue && (newline_pos = line_buffer.find(
                                   '\n', consumed)) != std::string::npos) {
      std::string line = line_buffer.substr(consumed, newline_pos - consumed);
      consumed = newline_pos + 1;
      if (!line.empty() && line.back() == '\r') {
        line.pop_back();
      }

      if (line.empty()) {
        flush_event();
        continue;
      }

      if (line.rfind(":", 0) == 0) {
        continue; // SSE comment
      }

      if (line.rfind("data:", 0) == 0) {
        std::string payload_line = line.substr(5);
        if (!payload_line.empty() && payload_line.front() == ' ') {
          payload_line.erase(payload_line.begin());
        }
        if (!event_data.empty()) {
          event_data.push_back('\n');
        }
        event_data += payload_line;
      } else if (line.rfind("event:", 0) == 0) {
        event_type = line.substr(6);
        if (!event_type.empty() && event_type.front() == ' ') {
          event_type.erase(event_type.begin());
        }
      }
    }

    if (consumed > 0) {
      line_buffer.erase(0, consumed);
      consumed = 0;
    }
  };

  post_json_stream_raw(
      cfg, path, payload,
      [&line_buffer, &process_available_lines,
       &should_continue](const char *data, std::size_t len) {
        line_buffer.reserve(line_buffer.size() + len);
        line_buffer.append(data, len);
        process_available_lines();
        return should_continue;
      },
      extra_headers);

  if (!line_buffer.empty()) {
    line_buffer.push_back('\n');
    process_available_lines();
  }

  if (should_continue) {
    flush_event();
  }

  if (!callback_error.empty()) {
    std::cerr << "[Adapter] Callback failed: " << callback_error << std::endl;
    throw std::runtime_error(callback_error);
  }
}

inline void
stream_sse_events(const AdapterConfig &cfg, const std::string &path,
                  const json &payload,
                  const std::function<bool(const std::string &)> &on_event_data,
                  const httplib::Headers &extra_headers = {}) {
  stream_sse_events_ex(
      cfg, path, payload,
      [&on_event_data](const SseEvent &event) {
        return on_event_data(event.data);
      },
      extra_headers);
}

inline json normalize_messages_for_provider(const json &messages) {
  if (!messages.is_array()) {
    throw std::runtime_error("messages must be an array");
  }

  json normalized = json::array();
  for (const auto &msg : messages) {
    if (!msg.is_object()) {
      continue;
    }

    std::string role = msg.value("role", "");
    if (role == "agent") {
      role = "assistant";
    }

    if (role != "system" && role != "user" && role != "assistant" &&
        role != "tool") {
      continue;
    }

    const std::string content = msg.value("content", "");
    const bool has_tool_calls = msg.contains("tool_calls") &&
                                msg["tool_calls"].is_array() &&
                                !msg["tool_calls"].empty();

    if (content.empty() && !has_tool_calls && role != "tool") {
      continue;
    }

    json out = {{"role", role}, {"content", content}};
    if (has_tool_calls) {
      out["tool_calls"] = msg["tool_calls"];
    }
    if (role == "tool" && msg.contains("tool_call_id") &&
        msg["tool_call_id"].is_string()) {
      out["tool_call_id"] = msg["tool_call_id"];
    }
    normalized.push_back(out);
  }

  if (normalized.empty()) {
    throw std::runtime_error(
        "messages array has no valid provider-compatible items");
  }
  return normalized;
}

class OpenAICompatibleAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "openai-compatible"; }

  ChatResponse call_chat(const AdapterConfig &cfg,
                         const ChatRequest &request) const override {
    const json normalized_messages =
        normalize_messages_for_provider(request.messages);

    json payload = {
        {"model", request.model.empty() ? cfg.model : request.model},
        {"messages", normalized_messages},
        {"stream", false}};

    if (request.sampling_params.is_object()) {
      for (auto it = request.sampling_params.begin();
           it != request.sampling_params.end(); ++it) {
        payload[it.key()] = it.value();
      }
    }

    if (request.max_tokens > 0) {
      payload["max_tokens"] = request.max_tokens;
    } else if (!payload.contains("max_tokens")) {
      payload["max_tokens"] = request.sampling_params.value("max_tokens", 512);
    }

    if (cfg.enable_tools && request.tools.is_array() &&
        !request.tools.empty()) {
      payload["tools"] = request.tools;
      if (!request.tool_choice.is_null()) {
        payload["tool_choice"] = request.tool_choice;
      } else {
        payload["tool_choice"] = "auto";
      }
    }

    if (!request.stop.empty()) {
      payload["stop"] = request.stop;
    } else if (!cfg.stop_tokens.empty()) {
      payload["stop"] = cfg.stop_tokens;
    }

    if (request.extra_body.is_object()) {
      for (auto it = request.extra_body.begin(); it != request.extra_body.end();
           ++it) {
        payload[it.key()] = it.value();
      }
    }

    const std::string body = post_json(cfg, cfg.chat_endpoint, payload);
    json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw std::runtime_error(
          extract_error_message(response_json["error"], "LLM provider error"));
    }
    if (!response_json.contains("choices") ||
        !response_json["choices"].is_array() ||
        response_json["choices"].empty()) {
      throw std::runtime_error("Invalid LLM response format");
    }

    const json choice = response_json["choices"][0];
    const json message = choice.value("message", json::object());

    ChatResponse result;
    if (message.contains("content") && message["content"].is_string()) {
      result.content = message["content"].get<std::string>();
    }
    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
      result.tool_calls = message["tool_calls"];
      result.has_tool_calls = true;
    }
    result.finish_reason = choice.value("finish_reason", std::string(""));
    result.usage = response_json.value("usage", json::object());
    result.raw = response_json;
    return result;
  }

  void call_chat_stream(
      const AdapterConfig &cfg, const ChatRequest &request,
      const std::function<void(const StreamChunk &)> &callback) const override {
    const json normalized_messages =
        normalize_messages_for_provider(request.messages);

    // 1. HARDENED MINIMALIST PAYLOAD
    json payload = {
        {"model", request.model.empty() ? cfg.model : request.model},
        {"messages", normalized_messages},
        {"stream", true}};

    // Only add non-empty sampling parameters
    if (request.sampling_params.is_object()) {
      for (auto it = request.sampling_params.begin();
           it != request.sampling_params.end(); ++it) {
        if (!it.value().is_null()) {
          payload[it.key()] = it.value();
        }
      }
    }

    if (request.max_tokens > 0) {
      payload["max_tokens"] = request.max_tokens;
    }

    // Only add tools if populated
    if (cfg.enable_tools && request.tools.is_array() &&
        !request.tools.empty()) {
      payload["tools"] = request.tools;
      payload["tool_choice"] =
          request.tool_choice.is_null() ? "auto" : request.tool_choice;
    }

    if (!request.stop.empty()) {
      payload["stop"] = request.stop;
    } else if (!cfg.stop_tokens.empty()) {
      payload["stop"] = cfg.stop_tokens;
    }

    struct ToolCallState {
      std::string id;
      std::string name;
      std::string arguments;
    };
    std::map<int, ToolCallState> tool_states;
    bool finished_sent = false;

    try {
      stream_sse_events(
          cfg, cfg.chat_endpoint, payload,
          [&](const std::string &event_data) -> bool {
            std::string trimmed = event_data;
            auto start = trimmed.find_first_not_of(" \t\n\r");
            if (start == std::string::npos) {
              return true; // Only whitespace, skip
            }
            auto end = trimmed.find_last_not_of(" \t\n\r");
            trimmed = trimmed.substr(start, end - start + 1);

            if (trimmed == "[DONE]")
              return true;

            json chunk;
            try {
              chunk = json::parse(trimmed);
            } catch (const std::exception &e) {
              std::cerr << "[Adapter] JSON Parse Error: " << e.what()
                        << " on data: " << trimmed << std::endl;
              return true; // Skip malformed lines instead of killing stream
            }

            if (chunk.contains("error")) {
              throw std::runtime_error(
                  extract_error_message(chunk["error"], "Stream Error"));
            }

            if (!chunk.contains("choices") || !chunk["choices"].is_array())
              return true;

            for (const auto &choice : chunk["choices"]) {
              const json &delta = choice.value("delta", json::object());

              if (delta.contains("content") && delta["content"].is_string()) {
                callback(StreamChunk{delta["content"].get<std::string>(),
                                     json::object(), false});
              }

              if (delta.contains("tool_calls") &&
                  delta["tool_calls"].is_array()) {
                for (const auto &tc : delta["tool_calls"]) {
                  int idx = tc.value("index", 0);
                  auto &state = tool_states[idx];

                  if (tc.contains("id") && tc["id"].is_string())
                    state.id = tc["id"].get<std::string>();
                  if (tc.contains("function") && tc["function"].is_object()) {
                    const auto &fn = tc["function"];
                    if (fn.contains("name") && fn["name"].is_string())
                      state.name = fn["name"].get<std::string>();
                    if (fn.contains("arguments") && fn["arguments"].is_string())
                      state.arguments += fn["arguments"].get<std::string>();
                  }
                }
              }

              std::string finish_reason;
              if (choice.contains("finish_reason") &&
                  choice["finish_reason"].is_string()) {
                finish_reason = choice["finish_reason"].get<std::string>();
              }

              if (!finish_reason.empty() && !finished_sent) {
                if (!tool_states.empty()) {
                  for (auto const &[idx, state] : tool_states) {
                    json tc_obj = {{"id", state.id},
                                   {"index", idx},
                                   {"type", "function"},
                                   {"function",
                                    {{"name", state.name},
                                     {"arguments", state.arguments}}}};
                    callback(StreamChunk{"", tc_obj, false});
                  }
                  tool_states.clear();
                }

                callback(StreamChunk{"", json::object(), true});
                finished_sent = true;
              }
            }
            return true;
          });
    } catch (const std::exception &e) {
      std::cerr << "[Adapter] Callback Exception: " << e.what() << std::endl;
      throw;
    }

    if (!finished_sent) {
      if (!tool_states.empty()) {
        for (auto const &[idx, state] : tool_states) {
          json tc_obj = {
              {"id", state.id},
              {"index", idx},
              {"type", "function"},
              {"function",
               {{"name", state.name}, {"arguments", state.arguments}}}};
          callback(StreamChunk{"", tc_obj, false});
        }
        tool_states.clear();
      }
      callback(StreamChunk{"", json::object(), true});
    }
  }
};

} // namespace velix::llm::adapters