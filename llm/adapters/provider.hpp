#pragma once

#include "../../vendor/nlohmann/json.hpp"

#include <memory>
#include <string>
#include <vector>

namespace velix::llm::adapters {

using json = nlohmann::json;

struct AdapterConfig {
  std::string active_adapter;
  std::string base_url;
  std::string host;
  std::string base_path;
  std::string chat_endpoint{"/chat/completions"};
  std::string model;
  int port{8033};
  bool use_https{false};
  int timeout_ms{60000};
  std::vector<std::string> stop_tokens;
};

class ProviderAdapter {
public:
  virtual ~ProviderAdapter() = default;
  virtual std::string provider_name() const = 0;
  virtual std::string call_chat(const AdapterConfig &cfg, const json &messages,
                                const json &sampling_params) const = 0;
};

std::unique_ptr<ProviderAdapter> make_adapter(const std::string &name);

} // namespace velix::llm::adapters
