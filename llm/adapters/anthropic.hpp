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
        } catch (...) {
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

class AnthropicAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "anthropic"; }

  ChatResponse call_chat(const AdapterConfig &cfg,
                         const ChatRequest &request) const override {
    json payload = {{"model", request.model.empty() ? cfg.model : request.model},
                    {"max_tokens", request.max_tokens > 0
                                       ? request.max_tokens
                                       : request.sampling_params.value("max_tokens", 512)},
                    {"stream", false},
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

    if (!system_prompt.empty()) {
      payload["system"] = system_prompt;
    }

    if (cfg.enable_tools && request.tools.is_array() && !request.tools.empty()) {
      json anthropic_tools = json::array();
      for (const auto &tool : request.tools) {
        const json converted = to_anthropic_tool_schema(tool);
        if (!converted.empty()) {
          anthropic_tools.push_back(converted);
        }
      }
      if (!anthropic_tools.empty()) {
        payload["tools"] = anthropic_tools;
        if (!request.tool_choice.is_null()) {
          if (request.tool_choice.is_string()) {
            payload["tool_choice"] = {{"type", request.tool_choice.get<std::string>()}};
          } else {
            payload["tool_choice"] = request.tool_choice;
          }
        } else {
          payload["tool_choice"] = {{"type", "auto"}};
        }
      }
    }

    if (request.extra_body.is_object()) {
      for (auto it = request.extra_body.begin(); it != request.extra_body.end();
           ++it) {
        payload[it.key()] = it.value();
      }
    }

    std::string endpoint = cfg.chat_endpoint;
    if (endpoint.empty() || endpoint == "/chat/completions") {
      endpoint = "/v1/messages";
    }

    httplib::Headers headers;
    if (!cfg.api_key.empty()) {
      headers.emplace("x-api-key", cfg.api_key);
      headers.emplace("anthropic-version", "2023-06-01");
    }

    const std::string body = post_json(cfg, endpoint, payload, headers);
    const json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw std::runtime_error(
          extract_error_message(response_json["error"], "Anthropic provider error"));
    }

    ChatResponse response;
    response.finish_reason = response_json.value("stop_reason", std::string(""));
    response.usage = response_json.value("usage", json::object());
    response.raw = response_json;

    if (!response_json.contains("content") || !response_json["content"].is_array()) {
      return response;
    }

    json normalized_tool_calls = json::array();
    std::string collected_text;
    for (const auto &item : response_json["content"]) {
      const std::string type = item.value("type", std::string(""));
      if (type == "text") {
        collected_text += item.value("text", std::string(""));
      } else if (type == "tool_use") {
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
    return response;
  }

  void call_chat_stream(
      const AdapterConfig &cfg, const ChatRequest &request,
      const std::function<void(const StreamChunk &)> &callback) const override {
    json payload = {{"model", request.model.empty() ? cfg.model : request.model},
                    {"max_tokens", request.max_tokens > 0
                                       ? request.max_tokens
                                       : request.sampling_params.value("max_tokens", 512)},
                    {"stream", true},
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

    if (!system_prompt.empty()) {
      payload["system"] = system_prompt;
    }

    if (cfg.enable_tools && request.tools.is_array() && !request.tools.empty()) {
      json anthropic_tools = json::array();
      for (const auto &tool : request.tools) {
        const json converted = to_anthropic_tool_schema(tool);
        if (!converted.empty()) {
          anthropic_tools.push_back(converted);
        }
      }
      if (!anthropic_tools.empty()) {
        payload["tools"] = anthropic_tools;
        if (!request.tool_choice.is_null()) {
          if (request.tool_choice.is_string()) {
            payload["tool_choice"] = {{"type", request.tool_choice.get<std::string>()}};
          } else {
            payload["tool_choice"] = request.tool_choice;
          }
        } else {
          payload["tool_choice"] = {{"type", "auto"}};
        }
      }
    }

    if (request.extra_body.is_object()) {
      for (auto it = request.extra_body.begin(); it != request.extra_body.end();
           ++it) {
        payload[it.key()] = it.value();
      }
    }

    std::string endpoint = cfg.chat_endpoint;
    if (endpoint.empty() || endpoint == "/chat/completions") {
      endpoint = "/v1/messages";
    }

    httplib::Headers headers;
    if (!cfg.api_key.empty()) {
      headers.emplace("x-api-key", cfg.api_key);
      headers.emplace("anthropic-version", "2023-06-01");
    }

    // Accumulated tool call deltas keyed by index
    std::map<int, json> tool_call_acc;
    bool finished_sent = false;
    std::string stream_error;
    stream_sse_events_ex(
        cfg, endpoint, payload,
        [&callback, &finished_sent, &stream_error,
         &tool_call_acc](const SseEvent &event) {
          std::string trimmed = event.data;
          // The core stream_sse_events_ex already trims, 
          // but we guard against [DONE] or empty just in case.
          if (trimmed == "[DONE]" || trimmed.empty()) {
            return true;
          }

          json event_json;
          try {
            event_json = json::parse(trimmed);
          } catch (...) {
            return true;
          }

          std::string event_type = event.event;
          if (event_type.empty()) {
            if (event_json.contains("type") && event_json["type"].is_string()) {
               event_type = event_json["type"].get<std::string>();
            }
          }

          if (event_type == "error" && event_json.contains("error")) {
            stream_error = extract_error_message(event_json["error"],
                                                 "Anthropic stream provider error");
            return false;
          }

          if (event_type == "content_block_start") {
            const json block = event_json.value("content_block", json::object());
            if (block.contains("type") && block["type"].is_string() && 
                block["type"].get<std::string>() == "tool_use") {
              const int index = event_json.value("index", 0);
              const std::string id = block.contains("id") && block["id"].is_string() 
                                     ? block["id"].get<std::string>() : "";
              const std::string name = block.contains("name") && block["name"].is_string() 
                                       ? block["name"].get<std::string>() : "";
              tool_call_acc[index] = {{"index", index},
                                      {"id", id},
                                      {"type", "function"},
                                      {"function", {{"name", name}, {"arguments", ""}}}};
            }
            return true;
          }

          if (event_type == "content_block_delta") {
            const int index = event_json.value("index", 0);
            const json delta = event_json.value("delta", json::object());
            const std::string delta_type = delta.contains("type") && delta["type"].is_string() 
                                           ? delta["type"].get<std::string>() : "";
            if (delta_type == "text_delta") {
              const std::string text = delta.contains("text") && delta["text"].is_string() 
                                       ? delta["text"].get<std::string>() : "";
              if (!text.empty()) {
                callback(StreamChunk{text, json::object(), false});
              }
              return true;
            }
            if (delta_type == "input_json_delta") {
              const std::string partial = delta.contains("partial_json") && delta["partial_json"].is_string() 
                                          ? delta["partial_json"].get<std::string>() : "";
              tool_call_acc[index]["function"]["arguments"] =
                  tool_call_acc[index]["function"].value("arguments", std::string("")) + partial;
              return true;
            }
            return true;
          }

          if (event_type == "message_stop" && !finished_sent) {
            // Flush any accumulated tool calls before the final chunk
            for (auto &[idx, tc] : tool_call_acc) {
              callback(StreamChunk{"", tc, false});
            }
            tool_call_acc.clear();
            
            callback(StreamChunk{"", json::object(), true});
            finished_sent = true;
            return false;
          }

          return true;
        },
        headers);

    if (!stream_error.empty()) {
      throw std::runtime_error(stream_error);
    }

    if (!finished_sent) {
      callback(StreamChunk{"", json::object(), true});
    }
  }
};

} // namespace velix::llm::adapters
