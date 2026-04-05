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

class OllamaAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "ollama"; }

  ChatResponse call_chat(const AdapterConfig &cfg,
                         const ChatRequest &request) const override {
    const json normalized_messages = normalize_messages_for_provider(request.messages);
    
    json options = json::object();
    const json &sp = request.sampling_params;
    
    if (sp.contains("temperature") && sp["temperature"].is_number()) options["temperature"] = sp["temperature"];
    else options["temperature"] = 0.7;
    
    if (sp.contains("top_p") && sp["top_p"].is_number()) options["top_p"] = sp["top_p"];
    else options["top_p"] = 0.9;
    
    if (sp.contains("top_k") && sp["top_k"].is_number()) options["top_k"] = sp["top_k"];
    else options["top_k"] = 40;
    
    if (request.max_tokens > 0) options["num_predict"] = request.max_tokens;
    else if (sp.contains("max_tokens") && sp["max_tokens"].is_number()) options["num_predict"] = sp["max_tokens"];
    else options["num_predict"] = 512;

    json payload = {
        {"model", request.model.empty() ? cfg.model : request.model},
        {"messages", normalized_messages},
        {"stream", false},
        {"options", options}
    };

    if (!request.stop.empty()) {
      payload["options"]["stop"] = request.stop;
    } else if (!cfg.stop_tokens.empty()) {
      payload["options"]["stop"] = cfg.stop_tokens;
    }

    if (cfg.enable_tools && request.tools.is_array() && !request.tools.empty()) {
      payload["tools"] = request.tools;
      payload["tool_choice"] = request.tool_choice.is_null() ? "auto" : request.tool_choice;
    }

    if (request.extra_body.is_object()) {
      for (auto it = request.extra_body.begin(); it != request.extra_body.end();
           ++it) {
        payload[it.key()] = it.value();
      }
    }

    std::string endpoint = cfg.chat_endpoint;
    if (endpoint.empty() || endpoint == "/chat/completions") {
      endpoint = "/api/chat";
    }

    const std::string body = post_json(cfg, endpoint, payload);
    json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw std::runtime_error(
          extract_error_message(response_json["error"], "Ollama provider error"));
    }

    if (!response_json.contains("message") || !response_json["message"].is_object()) {
      throw std::runtime_error("Invalid Ollama response format: missing message");
    }

    const json message = response_json["message"];

    ChatResponse result;
    if (message.contains("content") && message["content"].is_string()) {
      result.content = message["content"].get<std::string>();
    }

    if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
      json normalized_tcs = json::array();
      int idx = 0;
      for (const auto &tc : message["tool_calls"]) {
        normalized_tcs.push_back(normalize_ollama_tool_call(tc, idx++));
      }
      result.tool_calls = normalized_tcs;
      result.has_tool_calls = true;
    }

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
    const json normalized_messages = normalize_messages_for_provider(request.messages);
    
    json options = json::object();
    const json &sp = request.sampling_params;
    
    if (sp.contains("temperature") && sp["temperature"].is_number()) options["temperature"] = sp["temperature"];
    else options["temperature"] = 0.7;
    
    if (sp.contains("top_p") && sp["top_p"].is_number()) options["top_p"] = sp["top_p"];
    else options["top_p"] = 0.9;
    
    if (sp.contains("top_k") && sp["top_k"].is_number()) options["top_k"] = sp["top_k"];
    else options["top_k"] = 40;
    
    if (request.max_tokens > 0) options["num_predict"] = request.max_tokens;
    else if (sp.contains("max_tokens") && sp["max_tokens"].is_number()) options["num_predict"] = sp["max_tokens"];
    else options["num_predict"] = 512;

    json payload = {
        {"model", request.model.empty() ? cfg.model : request.model},
        {"messages", normalized_messages},
        {"stream", true},
        {"options", options}
    };

    if (!request.stop.empty()) {
      payload["options"]["stop"] = request.stop;
    } else if (!cfg.stop_tokens.empty()) {
      payload["options"]["stop"] = cfg.stop_tokens;
    }

    if (cfg.enable_tools && request.tools.is_array() && !request.tools.empty()) {
      payload["tools"] = request.tools;
      payload["tool_choice"] = request.tool_choice.is_null() ? "auto" : request.tool_choice;
    }

    if (request.extra_body.is_object()) {
      for (auto it = request.extra_body.begin(); it != request.extra_body.end();
           ++it) {
        payload[it.key()] = it.value();
      }
    }

    std::string endpoint = cfg.chat_endpoint;
    if (endpoint.empty() || endpoint == "/chat/completions") {
      endpoint = "/api/chat";
    }

    std::string line_buffer;
    bool finished_sent = false;
    std::string stream_error;

    auto process_line = [&callback, &finished_sent, &stream_error](std::string line) {
      if (!stream_error.empty() || finished_sent) {
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
      } catch (...) {
        return;
      }

      if (chunk_json.contains("error")) {
        stream_error = extract_error_message(chunk_json["error"],
                                             "Ollama stream provider error");
        return;
      }

      const json message = chunk_json.value("message", json::object());
      if (message.contains("content") && message["content"].is_string()) {
        const std::string delta = message["content"].get<std::string>();
        if (!delta.empty()) {
          try {
            callback(StreamChunk{delta, json::object(), false});
          } catch (const std::exception &e) {
            stream_error = std::string("Ollama stream callback error: ") + e.what();
            return;
          } catch (...) {
            stream_error = "Ollama stream callback error";
            return;
          }
        }
      }

      if (message.contains("tool_calls") && message["tool_calls"].is_array()) {
        int idx = 0;
        for (const auto &tc : message["tool_calls"]) {
          if (tc.is_object()) {
            try {
              callback(StreamChunk{"", normalize_ollama_tool_call(tc, idx++), false});
            } catch (const std::exception &e) {
              stream_error = std::string("Ollama stream callback error: ") + e.what();
              return;
            } catch (...) {
              stream_error = "Ollama stream callback error";
              return;
            }
          }
        }
      }

      if (chunk_json.value("done", false)) {
        if (!finished_sent) {
          try {
            callback(StreamChunk{"", json::object(), true});
            finished_sent = true;
          } catch (const std::exception &e) {
            stream_error = std::string("Ollama stream callback error: ") + e.what();
            return;
          } catch (...) {
            stream_error = "Ollama stream callback error";
            return;
          }
        }
      }
    };

    post_json_stream_raw(
        cfg, endpoint, payload,
      [&line_buffer, &process_line](const char *data, std::size_t len) {
          line_buffer.reserve(line_buffer.size() + len);
          line_buffer.append(data, len);
          std::size_t newline_pos = std::string::npos;
          while ((newline_pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, newline_pos);
            line_buffer.erase(0, newline_pos + 1);
            process_line(line);
          }
          return true;
        });

    // Some servers close the stream without a trailing newline; process the
    // final buffered JSON object so the last token and done flag are not lost.
    if (!line_buffer.empty()) {
      process_line(line_buffer);
      line_buffer.clear();
    }

    if (!stream_error.empty()) {
      throw std::runtime_error(stream_error);
    }

    // Some implementations may end the stream without a final done chunk.
    // Emit completion here so callers don't wait indefinitely.
    if (!finished_sent) {
      callback(StreamChunk{"", json::object(), true});
    }
  }
};

} // namespace velix::llm::adapters
