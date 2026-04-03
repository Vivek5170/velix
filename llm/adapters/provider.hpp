#pragma once

#include "../../vendor/nlohmann/json.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace velix::llm::adapters {

using json = nlohmann::json;

struct AdapterConfig {
  std::string active_adapter;
  std::string base_url;
  std::string api_key;
  std::string host;
  std::string base_path;
  std::string chat_endpoint{"/chat/completions"};
  std::string model;
  int port{8033};
  bool use_https{false};
  int timeout_ms{60000};
  std::vector<std::string> stop_tokens;
  bool enable_tools{true};
  bool enable_streaming{true};
  int stream_idle_timeout_ms{0};
};

struct ChatRequest {
  std::string model;
  json messages = json::array();
  json tools = json::array();
  json tool_choice;
  json sampling_params = json::object();
  bool stream{false};
  int max_tokens{0};
  std::vector<std::string> stop;
  json extra_body = json::object();
};

struct ChatResponse {
  std::string content;
  json tool_calls = json::array();
  bool has_tool_calls{false};
  std::string finish_reason;
  json usage = json::object();
  json raw = json::object();
};

struct StreamChunk {
  std::string delta_text;
  json delta_tool_call = json::object();
  bool finished{false};
};

class ProviderAdapter {
public:
  virtual ~ProviderAdapter() = default;
  virtual std::string provider_name() const = 0;
  virtual ChatResponse call_chat(const AdapterConfig &cfg,
                                 const ChatRequest &request) const = 0;
  virtual void call_chat_stream(
      const AdapterConfig &cfg, const ChatRequest &request,
      const std::function<void(const StreamChunk &)> &callback) const {
    ChatRequest non_stream_request = request;
    non_stream_request.stream = false;
    const ChatResponse response = call_chat(cfg, non_stream_request);
    
    // Emit content if present
    if (!response.content.empty()) {
      callback(StreamChunk{response.content, json::object(), false});
    }
    
    // Emit tool calls if present
    for (const auto &tc : response.tool_calls) {
      if (tc.is_object()) {
        callback(StreamChunk{"", tc, false});
      }
    }
    
    // Send final chunk
    callback(StreamChunk{"", json::object(), true});
  }
};

std::unique_ptr<ProviderAdapter> make_adapter(const std::string &name);

} // namespace velix::llm::adapters
