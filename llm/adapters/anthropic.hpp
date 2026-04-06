#pragma once

#include "openai_compatible.hpp"

#include <string>

namespace velix::llm::adapters {

inline json to_anthropic_tool_schema(const json &openai_tool) {
  if (!openai_tool.is_object()) {
    return json::object();
  }
  if (!openai_tool.contains("function") || !openai_tool["function"].is_object()) {
    return json::object();
  }

  const json fn = openai_tool["function"];
  json params = fn.value("parameters", json::object());
  if (!params.contains("type")) {
    params["type"] = "object";
  }
  if (!params.contains("properties")) {
    params["properties"] = json::object();
  }

  return {{"name", fn.value("name", std::string(""))},
          {"description", fn.value("description", std::string(""))},
          {"input_schema", params}};
}

inline json to_anthropic_content_from_message(const json &message) {
  const std::string role = message.value("role", std::string(""));
  const std::string content = message.value("content", std::string(""));

  if (role == "assistant" && message.contains("tool_calls") &&
      message["tool_calls"].is_array() && !message["tool_calls"].empty()) {
    json content_items = json::array();
    if (!content.empty()) {
      content_items.push_back({{"type", "text"}, {"text", content}});
    }

    for (const auto &tool_call : message["tool_calls"]) {
      if (!tool_call.is_object()) {
        continue;
      }
      const json fn = tool_call.value("function", json::object());
      json parsed_args = json::object();
      if (fn.contains("arguments") && fn["arguments"].is_string()) {
        try {
          parsed_args = json::parse(fn["arguments"].get<std::string>());
        } catch (const nlohmann::json::parse_error &) {
          parsed_args = json::object();
        }
      }
      content_items.push_back(
          {{"type", "tool_use"},
           {"id", tool_call.value("id", std::string(""))},
           {"name", fn.value("name", std::string(""))},
           {"input", parsed_args}});
    }
    return content_items;
  }

  if (role == "tool") {
    return json::array(
        {{{"type", "tool_result"},
          {"tool_use_id", message.value("tool_call_id", std::string(""))},
          {"content", content}}});
  }

  return json::array({{{"type", "text"}, {"text", content}}});
}

inline void append_anthropic_messages(const ChatRequest &request, json &payload,
                                      std::string &system_prompt) {
  for (const auto &msg : request.messages) {
    if (!msg.is_object()) {
      continue;
    }

    const std::string role = msg.value("role", std::string(""));
    if (role == "system") {
      if (!system_prompt.empty()) {
        system_prompt += "\n";
      }
      system_prompt += msg.value("content", std::string(""));
      continue;
    }

    if (role != "user" && role != "assistant" && role != "tool") {
      continue;
    }

    payload["messages"].push_back(
        {{"role", role == "tool" ? "user" : role},
         {"content", to_anthropic_content_from_message(msg)}});
  }
}

inline void apply_anthropic_tools(const AdapterConfig &cfg,
                                  const ChatRequest &request, json &payload) {
  if (!(cfg.enable_tools && request.tools.is_array() && !request.tools.empty())) {
    return;
  }

  json anthropic_tools = json::array();
  for (const auto &tool : request.tools) {
    const json converted = to_anthropic_tool_schema(tool);
    if (!converted.empty()) {
      anthropic_tools.push_back(converted);
    }
  }

  if (anthropic_tools.empty()) {
    return;
  }

  payload["tools"] = anthropic_tools;
  if (request.tool_choice.is_null()) {
    payload["tool_choice"] = {{"type", "auto"}};
    return;
  }

  if (request.tool_choice.is_string()) {
    payload["tool_choice"] =
        {{"type", request.tool_choice.get<std::string>()}};
    return;
  }

  payload["tool_choice"] = request.tool_choice;
}

inline json build_anthropic_payload(const AdapterConfig &cfg,
                                    const ChatRequest &request, bool stream) {
  json payload = {{"model", request.model.empty() ? cfg.model : request.model},
                  {"max_tokens", request.max_tokens > 0
                                     ? request.max_tokens
                                     : request.sampling_params.value("max_tokens", 512)},
                  {"stream", stream},
                  {"messages", json::array()}};

  if (request.sampling_params.contains("temperature")) {
    payload["temperature"] = request.sampling_params["temperature"];
  } else if (request.sampling_params.contains("temp")) {
    payload["temperature"] = request.sampling_params["temp"];
  }

  if (request.sampling_params.contains("top_p")) {
    payload["top_p"] = request.sampling_params["top_p"];
  }

  if (!request.stop.empty()) {
    payload["stop_sequences"] = request.stop;
  }

  std::string system_prompt;
  append_anthropic_messages(request, payload, system_prompt);
  if (!system_prompt.empty()) {
    payload["system"] = system_prompt;
  }

  apply_anthropic_tools(cfg, request, payload);

  if (request.extra_body.is_object()) {
    for (auto it = request.extra_body.begin(); it != request.extra_body.end();
         ++it) {
      payload[it.key()] = it.value();
    }
  }
  return payload;
}

inline std::string resolve_anthropic_endpoint(const std::string &chat_endpoint) {
  if (chat_endpoint.empty() || chat_endpoint == "/chat/completions") {
    return "/v1/messages";
  }
  return chat_endpoint;
}

inline httplib::Headers build_anthropic_headers(const AdapterConfig &cfg) {
  httplib::Headers headers;
  if (!cfg.api_key.empty()) {
    headers.emplace("x-api-key", cfg.api_key);
    headers.emplace("anthropic-version", "2023-06-01");
  }
  return headers;
}

inline void normalize_anthropic_response_content(const json &response_json,
                                                 ChatResponse &response) {
  if (!response_json.contains("content") || !response_json["content"].is_array()) {
    return;
  }

  json normalized_tool_calls = json::array();
  std::string collected_text;
  for (const auto &item : response_json["content"]) {
    const std::string type = item.value("type", std::string(""));
    if (type == "text") {
      collected_text += item.value("text", std::string(""));
      continue;
    }

    if (type == "tool_use") {
      normalized_tool_calls.push_back(
          {{"id", item.value("id", std::string(""))},
           {"type", "function"},
           {"function",
            {{"name", item.value("name", std::string(""))},
             {"arguments", item.value("input", json::object()).dump()}}}});
    }
  }

  response.content = collected_text;
  response.tool_calls = normalized_tool_calls;
  response.has_tool_calls = !normalized_tool_calls.empty();
}

struct AnthropicStreamState {
  std::map<int, json> tool_call_acc;
  bool finished_sent = false;
  std::string stream_error;
};

inline bool process_anthropic_stream_event(
    const SseEvent &event,
    const std::function<void(const StreamChunk &)> &callback,
    AnthropicStreamState &state) {
  std::string trimmed = event.data;
  if (trimmed == "[DONE]" || trimmed.empty()) {
    return true;
  }

  json event_json;
  try {
    event_json = json::parse(trimmed);
  } catch (const nlohmann::json::parse_error &) {
    return true;
  }

  std::string event_type = event.event;
  if (event_type.empty() && event_json.contains("type") &&
      event_json["type"].is_string()) {
    event_type = event_json["type"].get<std::string>();
  }

  if (event_type == "error" && event_json.contains("error")) {
    state.stream_error = extract_error_message(event_json["error"],
                                               "Anthropic stream provider error");
    return false;
  }

  if (event_type == "content_block_start") {
    const json block = event_json.value("content_block", json::object());
    if (block.contains("type") && block["type"].is_string() &&
        block["type"].get<std::string>() == "tool_use") {
      const int index = event_json.value("index", 0);
      const std::string id = block.contains("id") && block["id"].is_string()
                                 ? block["id"].get<std::string>()
                                 : "";
      const std::string name =
          block.contains("name") && block["name"].is_string()
              ? block["name"].get<std::string>()
              : "";
      state.tool_call_acc[index] = {{"index", index},
                                    {"id", id},
                                    {"type", "function"},
                                    {"function", {{"name", name},
                                                   {"arguments", ""}}}};
    }
    return true;
  }

  if (event_type == "content_block_delta") {
    const int index = event_json.value("index", 0);
    const json delta = event_json.value("delta", json::object());
    const std::string delta_type =
        delta.contains("type") && delta["type"].is_string()
            ? delta["type"].get<std::string>()
            : "";
    if (delta_type == "text_delta") {
      const std::string text = delta.contains("text") && delta["text"].is_string()
                                   ? delta["text"].get<std::string>()
                                   : "";
      if (!text.empty()) {
        callback(StreamChunk{text, json::object(), false});
      }
      return true;
    }

    if (delta_type == "input_json_delta") {
      const std::string partial =
          delta.contains("partial_json") && delta["partial_json"].is_string()
              ? delta["partial_json"].get<std::string>()
              : "";
      state.tool_call_acc[index]["function"]["arguments"] =
          state.tool_call_acc[index]["function"].value("arguments",
                                                        std::string("")) +
          partial;
    }
    return true;
  }

  if (event_type == "message_stop" && !state.finished_sent) {
    for (auto &[idx, tc] : state.tool_call_acc) {
      (void)idx;
      callback(StreamChunk{"", tc, false});
    }
    state.tool_call_acc.clear();
    callback(StreamChunk{"", json::object(), true});
    state.finished_sent = true;
    return false;
  }

  return true;
}

class AnthropicAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "anthropic"; }

  ChatResponse call_chat(const AdapterConfig &cfg,
                         const ChatRequest &request) const override {
    const json payload = build_anthropic_payload(cfg, request, false);
    const std::string endpoint = resolve_anthropic_endpoint(cfg.chat_endpoint);
    const httplib::Headers headers = build_anthropic_headers(cfg);

    const std::string body = post_json(cfg, endpoint, payload, headers);
    const json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw AdapterException(
          extract_error_message(response_json["error"], "Anthropic provider error"));
    }

    ChatResponse response;
    response.finish_reason = response_json.value("stop_reason", std::string(""));
    response.usage = response_json.value("usage", json::object());
    response.raw = response_json;

    normalize_anthropic_response_content(response_json, response);
    return response;
  }

  void call_chat_stream(
      const AdapterConfig &cfg, const ChatRequest &request,
      const std::function<void(const StreamChunk &)> &callback) const override {
    const json payload = build_anthropic_payload(cfg, request, true);
    const std::string endpoint = resolve_anthropic_endpoint(cfg.chat_endpoint);
    const httplib::Headers headers = build_anthropic_headers(cfg);

    AnthropicStreamState state;
    stream_sse_events_ex(
        cfg, endpoint, payload,
        [&callback, &state](const SseEvent &event) {
          return process_anthropic_stream_event(event, callback, state);
        },
        headers);

    if (!state.stream_error.empty()) {
      throw AdapterException(state.stream_error);
    }

    if (!state.finished_sent) {
      callback(StreamChunk{"", json::object(), true});
    }
  }
};

} // namespace velix::llm::adapters
