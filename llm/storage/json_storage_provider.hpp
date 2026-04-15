#pragma once

#include "istorage_provider.hpp"

#include <mutex>
#include <string>

namespace velix::llm::storage {

class JsonStorageProvider final : public IStorageProvider {
public:
  explicit JsonStorageProvider(std::string storage_root = "memory/sessions");

  bool upsert_conversation(const json &conversation) override;
  std::optional<json> get_conversation(const std::string &convo_id) override;
  bool delete_conversation(const std::string &convo_id,
                           int creator_pid_hint) override;

  void delete_all_proc_convos(int creator_pid) override;
  std::vector<std::string> list_proc_convo_ids(int creator_pid) override;
  std::vector<int> list_proc_creator_pids() override;

  bool upsert_super_user(const std::string &super_user) override;
  bool delete_super_user(const std::string &super_user) override;
  std::vector<std::string> list_super_users() override;

  bool upsert_session_index_entry(const std::string &super_user,
                                  const std::string &session_id,
                                  const std::string &title) override;
  bool delete_session_index_entry(const std::string &super_user,
                                  const std::string &session_id) override;
  std::vector<json>
  list_session_index_entries(const std::string &super_user) override;

  bool append_snapshot(const std::string &session_id,
                       const json &snapshot) override;
  int snapshot_count(const std::string &session_id) override;
  bool delete_snapshots(const std::string &session_id) override;
  bool delete_snapshots_for_super_user(const std::string &super_user) override;

private:
  std::string user_convo_path(const std::string &session_id) const;
  std::string process_convo_path(int creator_pid,
                                 const std::string &convo_id) const;
  std::string infer_convo_path(const std::string &convo_id,
                               int creator_pid_hint) const;
  std::string session_index_path() const;
  std::string snapshot_path(const std::string &session_id, int n) const;
  json load_index_unlocked() const;
  bool save_index_unlocked(const json &idx) const;

  std::string storage_root_;
  std::mutex io_mutex_;
};

} // namespace velix::llm::storage
