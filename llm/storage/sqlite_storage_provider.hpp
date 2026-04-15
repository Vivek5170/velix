#pragma once

#include "istorage_provider.hpp"

#include <mutex>
#include <string>

struct sqlite3;

namespace velix::llm::storage {

class SqliteStorageProvider final : public IStorageProvider {
public:
  explicit SqliteStorageProvider(std::string db_path = ".velix/velix.db");
  ~SqliteStorageProvider() override;

  SqliteStorageProvider(const SqliteStorageProvider &) = delete;
  SqliteStorageProvider &operator=(const SqliteStorageProvider &) = delete;

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
  bool exec_sql(const std::string &sql);
  bool ensure_schema();
  bool open_db();
  void close_db();

  sqlite3 *db_{nullptr};
  std::string db_path_;
  std::mutex db_mutex_;
};

} // namespace velix::llm::storage
