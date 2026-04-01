#pragma once

#include "provider.hpp"

#include "../../vendor/httplib.h"

#include <algorithm>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace velix::llm::adapters {

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
      throw std::runtime_error("Invalid adapter base_url port: " + cfg.base_url);
    }
  }

  if (host.empty()) {
    throw std::runtime_error("LLM host is empty; check adapter host/base_url");
  }
}

inline std::string post_json(const AdapterConfig &cfg, const std::string &path,
                             const json &payload) {
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
    res = cli.Post(endpoint, payload.dump(), "application/json");
  } else {
    httplib::Client cli(host, port);
    cli.set_connection_timeout(timeout_sec, timeout_usec);
    cli.set_read_timeout(timeout_sec, timeout_usec);
    cli.set_write_timeout(timeout_sec, timeout_usec);
    res = cli.Post(endpoint, payload.dump(), "application/json");
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
  res = cli.Post(endpoint, payload.dump(), "application/json");
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
    if (content.empty()) {
      continue;
    }

    json out = {{"role", role}, {"content", content}};
    if (role == "tool" && msg.contains("tool_call_id") &&
        msg["tool_call_id"].is_string()) {
      out["tool_call_id"] = msg["tool_call_id"];
    }
    normalized.push_back(out);
  }

  if (normalized.empty()) {
    throw std::runtime_error("messages array has no valid provider-compatible items");
  }
  return normalized;
}

class OpenAICompatibleAdapter : public ProviderAdapter {
public:
  std::string provider_name() const override { return "openai-compatible"; }

  std::string call_chat(const AdapterConfig &cfg, const json &messages,
                        const json &sampling_params) const override {
    const json normalized_messages = normalize_messages_for_provider(messages);
    json payload = {{"model", cfg.model},
            {"messages", normalized_messages},
                    {"temperature", sampling_params.value("temp", 0.7)},
                    {"top_p", sampling_params.value("top_p", 0.9)},
                    {"max_tokens", sampling_params.value("max_tokens", 512)}};

    if (!cfg.stop_tokens.empty()) {
      payload["stop"] = cfg.stop_tokens;
    }

    const std::string body = post_json(cfg, cfg.chat_endpoint, payload);
    json response_json = json::parse(body);

    if (response_json.contains("error")) {
      throw std::runtime_error("LLM error: " + response_json["error"].dump());
    }
    if (!response_json.contains("choices") || !response_json["choices"].is_array() ||
        response_json["choices"].empty()) {
      throw std::runtime_error("Invalid LLM response format");
    }

    return response_json["choices"][0]["message"].value("content", "");
  }
};

} // namespace velix::llm::adapters
