#pragma once

#include "../../vendor/nlohmann/json.hpp"

#include <optional>
#include <string>
#include <vector>

namespace velix::llm::storage {

using json = nlohmann::json;

class IStorageProvider {
public:
  virtual ~IStorageProvider() = default;

  virtual bool upsert_conversation(const json &conversation) = 0;
  virtual std::optional<json> get_conversation(const std::string &convo_id) = 0;
  virtual bool delete_conversation(const std::string &convo_id,
                                   int creator_pid_hint) = 0;

  virtual void delete_all_proc_convos(int creator_pid) = 0;
  virtual std::vector<std::string> list_proc_convo_ids(int creator_pid) = 0;
  virtual std::vector<int> list_proc_creator_pids() = 0;

  // Session index and metadata (user sessions)
  virtual bool upsert_super_user(const std::string &super_user) = 0;
  virtual bool delete_super_user(const std::string &super_user) = 0;
  virtual std::vector<std::string> list_super_users() = 0;

  virtual bool upsert_session_index_entry(const std::string &super_user,
                                          const std::string &session_id,
                                          const std::string &title) = 0;
  virtual bool delete_session_index_entry(const std::string &super_user,
                                          const std::string &session_id) = 0;
  virtual std::vector<json>
  list_session_index_entries(const std::string &super_user) = 0;

  // Session snapshots
  virtual bool append_snapshot(const std::string &session_id,
                               const json &snapshot) = 0;
  virtual int snapshot_count(const std::string &session_id) = 0;
  virtual bool delete_snapshots(const std::string &session_id) = 0;
  virtual bool delete_snapshots_for_super_user(const std::string &super_user) = 0;
};

} // namespace velix::llm::storage
