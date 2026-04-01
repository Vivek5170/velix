#pragma once

#include "openai_compatible.hpp"

namespace velix::llm::adapters {

class OllamaAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "ollama"; }

  std::string call_chat(const AdapterConfig &cfg, const json &messages,
                        const json &sampling_params) const override {
    const json normalized_messages = normalize_messages_for_provider(messages);
    json payload = {
        {"model", cfg.model},
      {"messages", normalized_messages},
        {"stream", false},
        {"options",
         {{"temperature", sampling_params.value("temp", 0.7)},
          {"top_p", sampling_params.value("top_p", 0.9)},
          {"num_predict", sampling_params.value("max_tokens", 512)}}}};

    if (!cfg.stop_tokens.empty()) {
      payload["options"]["stop"] = cfg.stop_tokens;
    }

    std::string endpoint = cfg.chat_endpoint;
    if (endpoint.empty() || endpoint == "/chat/completions") {
      endpoint = "/api/chat";
    }

    const std::string body = post_json(cfg, endpoint, payload);
    json response_json = json::parse(body);

    if (response_json.contains("error") && response_json["error"].is_string()) {
      throw std::runtime_error("Ollama error: " + response_json["error"].get<std::string>());
    }

    if (!response_json.contains("message") || !response_json["message"].is_object()) {
      throw std::runtime_error("Invalid Ollama response format: missing message");
    }

    return response_json["message"].value("content", "");
  }
};

} // namespace velix::llm::adapters
