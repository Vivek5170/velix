#pragma once

#include "openai_compatible.hpp"

namespace velix::llm::adapters {

class LlamaCppAdapter : public OpenAICompatibleAdapter {
public:
  std::string provider_name() const override { return "llama.cpp"; }
};

} // namespace velix::llm::adapters
