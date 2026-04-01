#pragma once

#include "llama_cpp.hpp"
#include "ollama.hpp"

#include <algorithm>
#include <cctype>
#include <string>

namespace velix::llm::adapters {

inline std::string normalize_adapter_key(std::string key) {
  std::transform(key.begin(), key.end(), key.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return key;
}

inline std::unique_ptr<ProviderAdapter> make_adapter(const std::string &name) {
  const std::string key = normalize_adapter_key(name);

  if (key == "llama.cpp" || key == "llama_cpp" || key == "llama" ||
      key == "openai" || key == "openai-compatible" ||
      key == "openai_chat" || key == "openai-chat-completions") {
    return std::make_unique<LlamaCppAdapter>();
  }
  if (key == "ollama" || key == "ollama-chat" || key == "ollama_chat") {
    return std::make_unique<OllamaAdapter>();
  }
  return std::make_unique<OpenAICompatibleAdapter>();
}

} // namespace velix::llm::adapters
