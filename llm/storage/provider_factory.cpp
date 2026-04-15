#include "provider_factory.hpp"

#include "json_storage_provider.hpp"
#include "sqlite_storage_provider.hpp"

#include "../../vendor/nlohmann/json.hpp"

#include <fstream>

namespace velix::llm::storage {

namespace {

using json = nlohmann::json;

json load_storage_config() {
  for (const char *path : {"config/storage.json", "../config/storage.json",
                           "build/config/storage.json"}) {
    std::ifstream in(path);
    if (!in.is_open()) {
      continue;
    }
    try {
      json cfg;
      in >> cfg;
      return cfg;
    } catch (...) {
      return json::object();
    }
  }
  return json::object();
}

} // namespace

std::shared_ptr<IStorageProvider> make_storage_provider_from_config() {
  const json cfg = load_storage_config();
  const std::string backend = cfg.value("backend", std::string("json"));

  if (backend == "sqlite") {
    const std::string sqlite_path =
        cfg.value("sqlite_path", std::string(".velix/velix.db"));
    return std::make_shared<SqliteStorageProvider>(sqlite_path);
  }

  const std::string json_root =
      cfg.value("json_root", std::string("memory/sessions"));
  return std::make_shared<JsonStorageProvider>(json_root);
}

} // namespace velix::llm::storage
