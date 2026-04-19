#include "sqlite_storage_provider.hpp"

#include "../../utils/logger.hpp"

#include <filesystem>
#include <sqlite3.h>

namespace fs = std::filesystem;

namespace velix::llm::storage {

namespace {

json parse_json_or_default(const std::string &raw, const json &fallback) {
  if (raw.empty()) {
    return fallback;
  }
  try {
    return json::parse(raw);
  } catch (const std::exception &) {
    return fallback;
  }
}

} // namespace

SqliteStorageProvider::SqliteStorageProvider(std::string db_path)
    : db_path_(std::move(db_path)) {
  if (!open_db() || !ensure_schema()) {
    throw std::runtime_error("SqliteStorageProvider initialization failed");
  }
}

SqliteStorageProvider::~SqliteStorageProvider() { close_db(); }

bool SqliteStorageProvider::open_db() {
  std::scoped_lock lock(db_mutex_);
  try {
    fs::create_directories(fs::path(db_path_).parent_path());
  } catch (const fs::filesystem_error &) {
    // Directory creation failed, continue anyway
  }

  if (sqlite3_open(db_path_.c_str(), &db_) != SQLITE_OK) {
    LOG_ERROR_CTX("sqlite3_open failed for " + db_path_, "storage", "", -1,
                  "sqlite_open_error");
    return false;
  }

  sqlite3_busy_timeout(db_, 2000);
  if (!exec_sql("PRAGMA journal_mode=WAL;")) {
    return false;
  }
  if (!exec_sql("PRAGMA synchronous=NORMAL;")) {
    return false;
  }
  if (!exec_sql("PRAGMA foreign_keys=ON;")) {
    return false;
  }
  return true;
}

void SqliteStorageProvider::close_db() {
  std::scoped_lock lock(db_mutex_);
  if (db_) {
    sqlite3_close(db_);
    db_ = nullptr;
  }
}

bool SqliteStorageProvider::exec_sql(const std::string &sql) {
  char *err = nullptr;
  const int rc = sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err);
  if (rc != SQLITE_OK) {
    const std::string msg = err ? err : "unknown";
    if (err) {
      sqlite3_free(err);
    }
    LOG_ERROR_CTX("sqlite exec failed: " + msg, "storage", "", -1,
                  "sqlite_exec_error");
    return false;
  }
  return true;
}

bool SqliteStorageProvider::ensure_schema() {
  std::scoped_lock lock(db_mutex_);
  return exec_sql(
      "CREATE TABLE IF NOT EXISTS conversations ("
      "convo_id TEXT PRIMARY KEY,"
      "user_id TEXT,"
      "creator_pid INTEGER,"
      "state TEXT,"
      "created_at_ms INTEGER,"
      "last_activity_ms INTEGER,"
      "turn_count INTEGER,"
      "current_context_tokens INTEGER,"
      "total_tokens_used INTEGER,"
      "messages_json TEXT,"
      "metadata_json TEXT"
      ");"
      "CREATE TABLE IF NOT EXISTS super_users ("
      "super_user TEXT PRIMARY KEY"
      ");"
      "CREATE TABLE IF NOT EXISTS session_index ("
      "super_user TEXT NOT NULL,"
      "session_id TEXT NOT NULL PRIMARY KEY,"
      "title TEXT NOT NULL DEFAULT '',"
      "FOREIGN KEY(super_user) REFERENCES super_users(super_user) ON DELETE CASCADE"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_session_index_super_user "
      "ON session_index(super_user);"
      "CREATE TABLE IF NOT EXISTS session_snapshots ("
      "session_id TEXT NOT NULL,"
      "snapshot_n INTEGER NOT NULL,"
      "snapshot_ms INTEGER NOT NULL,"
      "messages_json TEXT NOT NULL,"
      "PRIMARY KEY(session_id, snapshot_n)"
      ");"
      "CREATE INDEX IF NOT EXISTS idx_session_snapshots_session_id "
      "ON session_snapshots(session_id);"
      "CREATE INDEX IF NOT EXISTS idx_conversations_creator_pid "
      "ON conversations(creator_pid);"
      "CREATE INDEX IF NOT EXISTS idx_conversations_last_activity "
      "ON conversations(last_activity_ms);");
}

bool SqliteStorageProvider::upsert_conversation(const json &conversation) {
  std::scoped_lock lock(db_mutex_);
  const char *sql =
      "INSERT INTO conversations("
      "convo_id,user_id,creator_pid,state,created_at_ms,last_activity_ms,"
      "turn_count,current_context_tokens,total_tokens_used,messages_json,metadata_json)"
      "VALUES(?,?,?,?,?,?,?,?,?,?,?) "
      "ON CONFLICT(convo_id) DO UPDATE SET "
      "user_id=excluded.user_id,"
      "creator_pid=excluded.creator_pid,"
      "state=excluded.state,"
      "created_at_ms=excluded.created_at_ms,"
      "last_activity_ms=excluded.last_activity_ms,"
      "turn_count=excluded.turn_count,"
      "current_context_tokens=excluded.current_context_tokens,"
      "total_tokens_used=excluded.total_tokens_used,"
      "messages_json=excluded.messages_json,"
      "metadata_json=excluded.metadata_json;";

  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }

  const std::string convo_id = conversation.value("convo_id", "");
  if (convo_id.empty()) {
    sqlite3_finalize(stmt);
    return false;
  }

  const std::string user_id = conversation.value("user_id", "");
  const int creator_pid = conversation.value("creator_pid", -1);
  const std::string state = conversation.value("state", "ACTIVE");
  const long created_at_ms = conversation.value("created_at_ms", 0L);
  const long last_activity_ms = conversation.value("last_activity_ms", 0L);
  const int turn_count = conversation.value("turn_count", 0);
  const long long current_context_tokens =
      conversation.value("current_context_tokens", static_cast<uint64_t>(0));
  const long long total_tokens_used =
      conversation.value("total_tokens_used", static_cast<uint64_t>(0));
  const std::string messages_json =
      conversation.value("messages", json::array()).dump();
  const std::string metadata_json =
      conversation.value("metadata", json::object()).dump();

  sqlite3_bind_text(stmt, 1, convo_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, user_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 3, creator_pid);
  sqlite3_bind_text(stmt, 4, state.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int64(stmt, 5, created_at_ms);
  sqlite3_bind_int64(stmt, 6, last_activity_ms);
  sqlite3_bind_int(stmt, 7, turn_count);
  sqlite3_bind_int64(stmt, 8, current_context_tokens);
  sqlite3_bind_int64(stmt, 9, total_tokens_used);
  sqlite3_bind_text(stmt, 10, messages_json.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 11, metadata_json.c_str(), -1, SQLITE_TRANSIENT);

  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

std::optional<json> SqliteStorageProvider::get_conversation(const std::string &convo_id) {
  std::scoped_lock lock(db_mutex_);
  const char *sql =
      "SELECT convo_id,user_id,creator_pid,state,created_at_ms,last_activity_ms,"
      "turn_count,current_context_tokens,total_tokens_used,messages_json,metadata_json "
      "FROM conversations WHERE convo_id=?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return std::nullopt;
  }
  sqlite3_bind_text(stmt, 1, convo_id.c_str(), -1, SQLITE_TRANSIENT);

  const int rc = sqlite3_step(stmt);
  if (rc != SQLITE_ROW) {
    sqlite3_finalize(stmt);
    return std::nullopt;
  }

  json convo;
  convo["convo_id"] =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
  convo["user_id"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
  convo["creator_pid"] = sqlite3_column_int(stmt, 2);
  convo["state"] = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 3));
  convo["created_at_ms"] = sqlite3_column_int64(stmt, 4);
  convo["last_activity_ms"] = sqlite3_column_int64(stmt, 5);
  convo["turn_count"] = sqlite3_column_int(stmt, 6);
  convo["current_context_tokens"] =
      static_cast<uint64_t>(sqlite3_column_int64(stmt, 7));
  convo["total_tokens_used"] =
      static_cast<uint64_t>(sqlite3_column_int64(stmt, 8));

  const char *messages_raw =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 9));
  const char *metadata_raw =
      reinterpret_cast<const char *>(sqlite3_column_text(stmt, 10));
  convo["messages"] = parse_json_or_default(messages_raw ? messages_raw : "", json::array());
  convo["metadata"] = parse_json_or_default(metadata_raw ? metadata_raw : "", json::object());

  sqlite3_finalize(stmt);
  return convo;
}

bool SqliteStorageProvider::delete_conversation(const std::string &convo_id,
                                                int creator_pid_hint) {
  std::scoped_lock lock(db_mutex_);
  const char *sql = (creator_pid_hint > 0)
                        ? "DELETE FROM conversations WHERE convo_id=? AND creator_pid=?;"
                        : "DELETE FROM conversations WHERE convo_id=?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, convo_id.c_str(), -1, SQLITE_TRANSIENT);
  if (creator_pid_hint > 0) {
    sqlite3_bind_int(stmt, 2, creator_pid_hint);
  }
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

void SqliteStorageProvider::delete_all_proc_convos(int creator_pid) {
  std::scoped_lock lock(db_mutex_);
  const char *sql = "DELETE FROM conversations WHERE creator_pid=?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return;
  }
  sqlite3_bind_int(stmt, 1, creator_pid);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);
}

std::vector<std::string> SqliteStorageProvider::list_proc_convo_ids(int creator_pid) {
  std::scoped_lock lock(db_mutex_);
  std::vector<std::string> ids;
  const char *sql =
      "SELECT convo_id FROM conversations WHERE creator_pid=? ORDER BY convo_id;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return ids;
  }
  sqlite3_bind_int(stmt, 1, creator_pid);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *id = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (id != nullptr) {
      ids.emplace_back(id);
    }
  }
  sqlite3_finalize(stmt);
  return ids;
}

std::vector<int> SqliteStorageProvider::list_proc_creator_pids() {
  std::scoped_lock lock(db_mutex_);
  std::vector<int> pids;
  const char *sql =
      "SELECT DISTINCT creator_pid FROM conversations WHERE creator_pid > 0 ORDER BY creator_pid;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return pids;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    pids.push_back(sqlite3_column_int(stmt, 0));
  }
  sqlite3_finalize(stmt);
  return pids;
}

bool SqliteStorageProvider::upsert_super_user(const std::string &super_user) {
  std::scoped_lock lock(db_mutex_);
  const char *sql =
      "INSERT INTO super_users(super_user) VALUES (?) "
      "ON CONFLICT(super_user) DO NOTHING;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, super_user.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool SqliteStorageProvider::delete_super_user(const std::string &super_user) {
  std::scoped_lock lock(db_mutex_);
  const char *sql = "DELETE FROM super_users WHERE super_user=?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, super_user.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    return false;
  }

  const std::string pattern = super_user + "_s%";
  sqlite3_stmt *cleanup = nullptr;
  if (sqlite3_prepare_v2(
          db_, "DELETE FROM conversations WHERE convo_id LIKE ?;", -1, &cleanup,
          nullptr) == SQLITE_OK) {
    sqlite3_bind_text(cleanup, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(cleanup);
    sqlite3_finalize(cleanup);
  }

  if (sqlite3_prepare_v2(
          db_, "DELETE FROM session_snapshots WHERE session_id LIKE ?;", -1,
          &cleanup, nullptr) == SQLITE_OK) {
    sqlite3_bind_text(cleanup, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(cleanup);
    sqlite3_finalize(cleanup);
  }

  return true;
}

std::vector<std::string> SqliteStorageProvider::list_super_users() {
  std::scoped_lock lock(db_mutex_);
  std::vector<std::string> users;
  const char *sql = "SELECT super_user FROM super_users ORDER BY super_user;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return users;
  }
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char *u = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    if (u != nullptr) {
      users.emplace_back(u);
    }
  }
  sqlite3_finalize(stmt);
  return users;
}

bool SqliteStorageProvider::upsert_session_index_entry(
    const std::string &super_user, const std::string &session_id,
    const std::string &title) {
  std::scoped_lock lock(db_mutex_);
  sqlite3_exec(db_, "BEGIN IMMEDIATE;", nullptr, nullptr, nullptr);

  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_,
          "INSERT INTO super_users(super_user) VALUES (?) "
          "ON CONFLICT(super_user) DO NOTHING;",
          -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(stmt, 1, super_user.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (sqlite3_prepare_v2(
          db_,
          "INSERT INTO session_index(super_user,session_id,title) VALUES (?,?,?) "
          "ON CONFLICT(session_id) DO UPDATE SET "
          "super_user=excluded.super_user,title=excluded.title;",
          -1, &stmt, nullptr) != SQLITE_OK) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }
  sqlite3_bind_text(stmt, 1, super_user.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 2, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_text(stmt, 3, title.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);

  if (rc != SQLITE_DONE) {
    sqlite3_exec(db_, "ROLLBACK;", nullptr, nullptr, nullptr);
    return false;
  }

  sqlite3_exec(db_, "COMMIT;", nullptr, nullptr, nullptr);
  return true;
}

bool SqliteStorageProvider::delete_session_index_entry(
    const std::string &super_user, const std::string &session_id) {
  (void)super_user;
  std::scoped_lock lock(db_mutex_);
  const char *sql = "DELETE FROM session_index WHERE session_id=?;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

std::vector<json> SqliteStorageProvider::list_session_index_entries(
    const std::string &super_user) {
  std::scoped_lock lock(db_mutex_);
  std::vector<json> out;
  const char *sql =
      "SELECT session_id, title FROM session_index "
      "WHERE super_user=? ORDER BY session_id;";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(db_, sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return out;
  }
  sqlite3_bind_text(stmt, 1, super_user.c_str(), -1, SQLITE_TRANSIENT);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    auto sid = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 0));
    auto title = reinterpret_cast<const char *>(sqlite3_column_text(stmt, 1));
    out.push_back(json{{"session_id", sid ? sid : ""},
                       {"title", title ? title : ""}});
  }
  sqlite3_finalize(stmt);
  return out;
}

bool SqliteStorageProvider::append_snapshot(const std::string &session_id,
                                             const json &snapshot) {
  std::scoped_lock lock(db_mutex_);
  sqlite3_stmt *stmt = nullptr;
  const char *next_sql =
      "SELECT COALESCE(MAX(snapshot_n),0)+1 FROM session_snapshots "
      "WHERE session_id=?;";
  if (sqlite3_prepare_v2(db_, next_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  int next_n = 1;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    next_n = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);

  const char *ins_sql =
      "INSERT INTO session_snapshots(session_id,snapshot_n,snapshot_ms,messages_json) "
      "VALUES (?,?,?,?);";
  if (sqlite3_prepare_v2(db_, ins_sql, -1, &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  const long snapshot_ms = snapshot.value("snapshot_ms", 0L);
  const std::string messages = snapshot.value("messages", json::array()).dump();
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  sqlite3_bind_int(stmt, 2, next_n);
  sqlite3_bind_int64(stmt, 3, snapshot_ms);
  sqlite3_bind_text(stmt, 4, messages.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

int SqliteStorageProvider::snapshot_count(const std::string &session_id) {
  std::scoped_lock lock(db_mutex_);
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_, "SELECT COUNT(*) FROM session_snapshots WHERE session_id=?;", -1,
          &stmt, nullptr) != SQLITE_OK) {
    return 0;
  }
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  int count = 0;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    count = sqlite3_column_int(stmt, 0);
  }
  sqlite3_finalize(stmt);
  return count;
}

bool SqliteStorageProvider::delete_snapshots(const std::string &session_id) {
  std::scoped_lock lock(db_mutex_);
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_, "DELETE FROM session_snapshots WHERE session_id=?;", -1, &stmt,
          nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, session_id.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

bool SqliteStorageProvider::delete_snapshots_for_super_user(
    const std::string &super_user) {
  std::scoped_lock lock(db_mutex_);
  const std::string pattern = super_user + "_s%";
  sqlite3_stmt *stmt = nullptr;
  if (sqlite3_prepare_v2(
          db_, "DELETE FROM session_snapshots WHERE session_id LIKE ?;", -1,
          &stmt, nullptr) != SQLITE_OK) {
    return false;
  }
  sqlite3_bind_text(stmt, 1, pattern.c_str(), -1, SQLITE_TRANSIENT);
  const int rc = sqlite3_step(stmt);
  sqlite3_finalize(stmt);
  return rc == SQLITE_DONE;
}

} // namespace velix::llm::storage
