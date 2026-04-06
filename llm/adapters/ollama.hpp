#pragma once

#include "openai_compatible.hpp"

namespace velix::llm::adapters {

inline json normalize_ollama_tool_call(const json &ollama_tc, int index) {
  if (!ollama_tc.is_object() || !ollama_tc.contains("function")) {
    return ollama_tc;
  }
  const json fn = ollama_tc["function"];

  std::string name;
  if (fn.contains("name") && fn["name"].is_string()) {
    name = fn["name"].get<std::string>();
  }

  json arguments = json::object();
  if (fn.contains("arguments") && fn["arguments"].is_object()) {
    arguments = fn["arguments"];
  }

  return {
    {"id", "call_" + std::to_string(index) + "_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())},
    {"type", "function"},
    {"function", {
      {"name", name},
      {"arguments", arguments.dump()}
    }},
    {"index", index}
  };
}

inline json build_ollama_options(const ChatRequest &request) {
  json options = json::object();
  const json &sp = request.sampling_params;

  options["temperature"] =
      (sp.contains("temperature") && sp["temperature"].is_number())
          ? sp["temperature"]
          : json(0.7);
  options["top_p"] = (sp.contains("top_p") && sp["top_p"].is_number())
                         ? sp["top_p"]
                         : json(0.9);
  options["top_k"] = (sp.contains("top_k") && sp["top_k"].is_number())
                         ? sp["top_k"]
                         : json(40);

  if (request.max_tokens > 0) {
    options["num_predict"] = request.max_tokens;
  } else if (sp.contains("max_tokens") && sp["max_tokens"].is_number()) {
    options["num_predict"] = sp["max_tokens"];
  } else {
    options["num_predict"] = 512;
  }

  return options;
}

inline std::string resolve_ollama_endpoint(const std::string &chat_endpoint) {
  if (chat_endpoint.empty() || chat_endpoint == "/chat/completions") {
    return "/api/chat";
  }
  return chat_endpoint;
}

inline json build_ollama_payload(const AdapterConfig &cfg,
                                 const ChatRequest &request, bool stream) {
  const json normalized_messages =
      normalize_messages_for_provider(request.messages);
  json payload = {{"model", request.model.empty() ? cfg.model : request.model},
                  {"messages", normalized_messages},
                  {"stream", stream},
                  {"options", build_ollama_options(request)}};

  if (!request.stop.empty()) {
    payload["options"]["stop"] = request.stop;
  } else if (!cfg.stop_tokens.empty()) {
    payload["options"]["stop"] = cfg.stop_tokens;
  }

  if (cfg.enable_tools && request.tools.is_array() && !request.tools.empty()) {
    payload["tools"] = request.tools;
    payload["tool_choice"] =
        request.tool_choice.is_null() ? "auto" : request.tool_choice;
  }

  if (request.extra_body.is_object()) {
    for (auto it = request.extra_body.begin(); it != request.extra_body.end();
         ++it) {
      payload[it.key()] = it.value();
    }
  }

  return payload;
}

inline void populate_ollama_tool_calls(const json &message, ChatResponse &out) {
  if (!message.contains("tool_calls") || !message["tool_calls"].is_array()) {
    return;
  }

  json normalized_tcs = json::array();
  int idx = 0;
  for (const auto &tc : message["tool_calls"]) {
    normalized_tcs.push_back(normalize_ollama_tool_call(tc, idx++));
  }
  out.tool_calls = normalized_tcs;
  out.has_tool_calls = true;
}

struct OllamaStreamState {
  bool finished_sent = false;
  std::string stream_error;
};

inline void report_ollama_callback_error(const std::function<void(const StreamChunk &)> &callback,
                                         const StreamChunk &chunk,
                                         std::string &stream_error) {
  try {
    callback(chunk);
  } catch (const std::exception &e) {
    stream_error = std::string("Ollama stream callback error: ") + e.what();
  }
}

inline void process_ollama_stream_line(
    const std::function<void(const StreamChunk &)> &callback,
    OllamaStreamState &state, std::string line) {
  if (!state.stream_error.empty() || state.finished_sent) {
    return;
  }

  if (!line.empty() && line.back() == '\r') {
    line.pop_back();
  }
  if (line.empty()) {
    return;
  }

  json chunk_json;
  try {
    chunk_json = json::parse(line);
  } catch (const nlohmann::json::parse_error &) {
    return;
  }

  if (chunk_json.contains("error")) {
    state.stream_error =
        extract_error_message(chunk_json["error"], "Ollama stream provider error");
    return;
  }

  const json message = chunk_json.value("message", json::object());
  if (message.contains("content") && message["content"].is_string()) {
    const auto delta = message["content"].get<std::string>();
    if (!delta.empty()) {
      report_ollama_callback_error(
          callback, StreamChunk{delta, json::object(), false}, state.stream_error);
      if (!state.stream_error.empty()) {
        return;
      }
    }
  }

  if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
    int idx = 0;
    for (const auto &tc : message["tool_calls"]) {
      if (!tc.is_object()) {
        continue;
      }
      report_ollama_callback_error(
          callback, StreamChunk{"", normalize_ollama_tool_call(tc, idx++), false},
          state.stream_error);
      if (!state.stream_error.empty()) {
        return;
      }
    }
  }

  if (chunk_json.value("done", false) && !state.finished_sent) {
    report_ollama_callback_error(callback,
                                 StreamChunk{"", json::object(), true},
                                 state.stream_error);
    if (state.stream_error.empty()) {
      state.finished_sent = true;
    }
  }
}

class OllamaAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "ollama"; }

  ChatResponse call_chat(const AdapterConfig &cfg,
                         const ChatRequest &request) const override {
    const json payload = build_ollama_payload(cfg, request, false);
    const std::string endpoint = resolve_ollama_endpoint(cfg.chat_endpoint);

    const std::string body = post_json(cfg, endpoint, payload);
    const json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw AdapterException(
          extract_error_message(response_json["error"], "Ollama provider error"));
    }

    if (!response_json.contains("message") || !response_json["message"].is_object()) {
      throw AdapterException("Invalid Ollama response format: missing message");
    }

    const json message = response_json["message"];

    ChatResponse result;
    if (message.contains("content") && message["content"].is_string()) {
      result.content = message["content"].get<std::string>();
    }

    populate_ollama_tool_calls(message, result);

    if (response_json.contains("done_reason") && response_json["done_reason"].is_string()) {
      result.finish_reason = response_json["done_reason"].get<std::string>();
    }

    result.usage = {
        {"prompt_tokens", response_json.value("prompt_eval_count", 0)},
        {"completion_tokens", response_json.value("eval_count", 0)}};
    result.raw = response_json;
    return result;
  }

  void call_chat_stream(
      const AdapterConfig &cfg, const ChatRequest &request,
      const std::function<void(const StreamChunk &)> &callback) const override {
    const json payload = build_ollama_payload(cfg, request, true);
    const std::string endpoint = resolve_ollama_endpoint(cfg.chat_endpoint);

    std::string line_buffer;
    OllamaStreamState state;

    post_json_stream_raw(
        cfg, endpoint, payload,
      [&line_buffer, &callback, &state](const char *data, std::size_t len) {
          line_buffer.reserve(line_buffer.size() + len);
          line_buffer.append(data, len);
          std::size_t newline_pos = std::string::npos;
          while (true) {
            newline_pos = line_buffer.find('\n');
            if (newline_pos == std::string::npos) {
              break;
            }
            std::string line = line_buffer.substr(0, newline_pos);
            line_buffer.erase(0, newline_pos + 1);
            process_ollama_stream_line(callback, state, std::move(line));
          }
          return true;
        });

    // Some servers close the stream without a trailing newline; process the
    // final buffered JSON object so the last token and done flag are not lost.
    if (!line_buffer.empty()) {
      process_ollama_stream_line(callback, state, std::move(line_buffer));
      line_buffer.clear();
    }

    if (!state.stream_error.empty()) {
      throw AdapterException(state.stream_error);
    }

    // Some implementations may end the stream without a final done chunk.
    // Emit completion here so callers don't wait indefinitely.
    if (!state.finished_sent) {
      callback(StreamChunk{"", json::object(), true});
    }
  }
};

} // namespace velix::llm::adapters
