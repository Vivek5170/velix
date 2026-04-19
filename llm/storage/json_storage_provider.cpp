#include "json_storage_provider.hpp"

#include "../session_manager.hpp"
#include "../../utils/logger.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>

namespace fs = std::filesystem;

namespace velix::llm::storage {

JsonStorageProvider::JsonStorageProvider(std::string storage_root)
    : storage_root_(std::move(storage_root)) {
  try {
    fs::create_directories(fs::path(storage_root_) / "users");
    fs::create_directories(fs::path(storage_root_) / "procs");
  } catch (const fs::filesystem_error &e) {
    LOG_ERROR_CTX(std::string("JsonStorageProvider init failed: ") + e.what(),
                  "storage", "", -1, "json_storage_init_error");
  }
}

std::string JsonStorageProvider::session_index_path() const {
  return (fs::path(storage_root_) / "session_index.json").string();
}

std::string JsonStorageProvider::snapshot_path(const std::string &session_id,
                                               int n) const {
  const std::string super_user = velix::llm::SessionManager::extract_super_user(session_id);
  const fs::path root = fs::path(storage_root_) / "users" / super_user / session_id;
  return (root / (session_id + "_h" + std::to_string(n) + ".json")).string();
}

json JsonStorageProvider::load_index_unlocked() const {
  const std::string primary = session_index_path();
  const std::string legacy =
      (fs::path(storage_root_).parent_path() / "session_index.json").string();

  for (const std::string &path : {primary, legacy}) {
    if (!fs::exists(path)) {
      continue;
    }
    try {
      std::ifstream in(path);
      json idx;
      in >> idx;
      if (!idx.contains("users") || !idx["users"].is_object()) {
        idx["users"] = json::object();
      }
      return idx;
    } catch (const std::exception &) {
      // Failed to parse index file, try next path
      continue;
    }
  }
  return json{{"users", json::object()}};
}

bool JsonStorageProvider::save_index_unlocked(const json &idx) const {
  try {
    const fs::path path(session_index_path());
    fs::create_directories(path.parent_path());
    std::ofstream out(path.string());
    if (!out.is_open()) {
      return false;
    }
    out << idx.dump(2);
    return true;
  } catch (const std::exception &) {
    // Failed to save index file
    return false;
  }
}

std::string JsonStorageProvider::user_convo_path(const std::string &session_id) const {
  const std::string super_user = velix::llm::SessionManager::extract_super_user(session_id);
  const fs::path session_dir = fs::path(storage_root_) / "users" / super_user / session_id;
  return (session_dir / (session_id + ".json")).string();
}

std::string JsonStorageProvider::process_convo_path(int creator_pid,
                                                    const std::string &convo_id) const {
  const fs::path session_dir =
      fs::path(storage_root_) / "procs" / std::to_string(creator_pid) / convo_id;
  return (session_dir / (convo_id + ".json")).string();
}

std::string JsonStorageProvider::infer_convo_path(const std::string &convo_id,
                                                  int creator_pid_hint) const {
  if (velix::llm::SessionManager::is_session_id(convo_id)) {
    return user_convo_path(convo_id);
  }

  if (creator_pid_hint > 0) {
    return process_convo_path(creator_pid_hint, convo_id);
  }

  if (convo_id.size() > 5 && convo_id.rfind("proc_", 0) == 0) {
    const std::string rest = convo_id.substr(5);
    const auto sep = rest.find('_');
    if (sep != std::string::npos) {
      try {
        const int creator_pid = std::stoi(rest.substr(0, sep));
        return process_convo_path(creator_pid, convo_id);
      } catch (const std::invalid_argument &) {
        // Failed to parse process ID
      } catch (const std::out_of_range &) {
        // Process ID out of range
      }
    }
  }

  return "";
}

bool JsonStorageProvider::upsert_conversation(const json &conversation) {
  std::scoped_lock lock(io_mutex_);
  try {
    const std::string convo_id = conversation.value("convo_id", "");
    const int creator_pid = conversation.value("creator_pid", -1);
    if (convo_id.empty()) {
      return false;
    }

    const std::string path = (creator_pid > 0) ? process_convo_path(creator_pid, convo_id)
                                                : user_convo_path(convo_id);
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path);
    if (!out.is_open()) {
      return false;
    }
    out << conversation.dump(2);
    return true;
  } catch (const std::exception &e) {
    LOG_ERROR_CTX(std::string("JsonStorageProvider upsert failed: ") + e.what(),
                  "storage", "", -1, "json_storage_upsert_error");
    return false;
  }
}

std::optional<json> JsonStorageProvider::get_conversation(const std::string &convo_id) {
  std::scoped_lock lock(io_mutex_);
  try {
    const std::string path = infer_convo_path(convo_id, -1);
    if (path.empty() || !fs::exists(path)) {
      return std::nullopt;
    }
    std::ifstream in(path);
    if (!in.is_open()) {
      return std::nullopt;
    }
    json convo;
    in >> convo;
    return convo;
  } catch (const std::exception &) {
    // Failed to load conversation
    return std::nullopt;
  }
}

bool JsonStorageProvider::delete_conversation(const std::string &convo_id,
                                              int creator_pid_hint) {
  std::scoped_lock lock(io_mutex_);
  try {
    const std::string path = infer_convo_path(convo_id, creator_pid_hint);
    if (path.empty()) {
      return false;
    }
    if (fs::exists(path)) {
      fs::remove(path);
    }
    return true;
  } catch (const fs::filesystem_error &) {
    // Failed to delete conversation
    return false;
  }
}

void JsonStorageProvider::delete_all_proc_convos(int creator_pid) {
  std::scoped_lock lock(io_mutex_);
  try {
    const fs::path pid_dir = fs::path(storage_root_) / "procs" / std::to_string(creator_pid);
    if (fs::exists(pid_dir)) {
      fs::remove_all(pid_dir);
    }
  } catch (const fs::filesystem_error &e) {
    LOG_ERROR_CTX(std::string("JsonStorageProvider delete_all_proc_convos failed: ") +
                      e.what(),
                  "storage", "", creator_pid, "json_storage_delete_pid_error");
  }
}

std::vector<std::string> JsonStorageProvider::list_proc_convo_ids(int creator_pid) {
  std::scoped_lock lock(io_mutex_);
  std::vector<std::string> result;
  try {
    const fs::path pid_dir = fs::path(storage_root_) / "procs" / std::to_string(creator_pid);
    if (!fs::exists(pid_dir)) {
      return result;
    }
    for (const auto &entry : fs::directory_iterator(pid_dir)) {
      if (entry.is_directory()) {
        result.push_back(entry.path().filename().string());
      }
    }
  } catch (const fs::filesystem_error &e) {
    LOG_ERROR_CTX(std::string("JsonStorageProvider list_proc_convo_ids failed: ") +
                      e.what(),
                  "storage", "", creator_pid, "json_storage_list_pid_error");
  }
  return result;
}

std::vector<int> JsonStorageProvider::list_proc_creator_pids() {
  std::scoped_lock lock(io_mutex_);
  std::vector<int> pids;
  try {
    const fs::path proc_root = fs::path(storage_root_) / "procs";
    if (!fs::exists(proc_root) || !fs::is_directory(proc_root)) {
      return pids;
    }
    for (const auto &entry : fs::directory_iterator(proc_root)) {
      if (!entry.is_directory()) {
        continue;
      }
      try {
        pids.push_back(std::stoi(entry.path().filename().string()));
      } catch (const std::invalid_argument &) {
        // Invalid directory name format, skip
      } catch (const std::out_of_range &) {
        // PID out of range, skip
      }
    }
  } catch (const fs::filesystem_error &e) {
    LOG_ERROR_CTX(std::string("JsonStorageProvider list_proc_creator_pids failed: ") +
                      e.what(),
                  "storage", "", -1, "json_storage_list_pids_error");
  }
  return pids;
}

bool JsonStorageProvider::upsert_super_user(const std::string &super_user) {
  std::scoped_lock lock(io_mutex_);
  try {
    fs::create_directories(fs::path(storage_root_) / "users" / super_user);
    fs::create_directories(fs::path(storage_root_).parent_path() / "agentfiles" /
                           super_user);

    json idx = load_index_unlocked();
    if (!idx["users"].contains(super_user) || !idx["users"][super_user].is_array()) {
      idx["users"][super_user] = json::array();
    }
    return save_index_unlocked(idx);
  } catch (const std::exception &) {
    // Failed to upsert super user
    return false;
  }
}

void JsonStorageProvider::delete_session_snapshots_internal(const std::string &session_id) {
  int n = 1;
  while (true) {
    const fs::path snap(snapshot_path(session_id, n));
    if (!fs::exists(snap)) {
      break;
    }
    fs::remove(snap);
    ++n;
  }
}

void JsonStorageProvider::list_index_entries_from_fs(const std::string &super_user,
                                                     std::vector<json> &out) const {
  const fs::path user_root = fs::path(storage_root_) / "users" / super_user;
  if (!fs::exists(user_root) || !fs::is_directory(user_root)) {
    return;
  }
  for (const auto &entry : fs::directory_iterator(user_root)) {
    if (!entry.is_directory()) {
      continue;
    }
    const std::string sid = entry.path().filename().string();
    if (!sid.empty()) {
      out.push_back(json{{"session_id", sid}, {"title", ""}});
    }
  }
}

bool JsonStorageProvider::delete_super_user(const std::string &super_user) {
  std::scoped_lock lock(io_mutex_);
  bool existed = false;
  try {
    json idx = load_index_unlocked();
    if (idx["users"].contains(super_user)) {
      idx["users"].erase(super_user);
      existed = true;
    }
    save_index_unlocked(idx);

    const fs::path user_root = fs::path(storage_root_) / "users" / super_user;
    if (fs::exists(user_root)) {
      fs::remove_all(user_root);
      existed = true;
    }

    const fs::path agent_root = fs::path(storage_root_).parent_path() /
                                "agentfiles" / super_user;
    if (fs::exists(agent_root)) {
      fs::remove_all(agent_root);
      existed = true;
    }

    const fs::path user_root_for_snaps = fs::path(storage_root_) / "users" / super_user;
    if (!fs::exists(user_root_for_snaps) || !fs::is_directory(user_root_for_snaps)) {
      return existed;
    }

    for (const auto &entry : fs::directory_iterator(user_root_for_snaps)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string sid = entry.path().filename().string();
      delete_session_snapshots_internal(sid);
      existed = true;
    }
  } catch (const fs::filesystem_error &) {
    // Failed to delete super user
  }
  return existed;
}

std::vector<std::string> JsonStorageProvider::list_super_users() {
  std::scoped_lock lock(io_mutex_);
  std::vector<std::string> users;
  try {
    json idx = load_index_unlocked();
    if (idx.contains("users") && idx["users"].is_object()) {
      for (auto it = idx["users"].begin(); it != idx["users"].end(); ++it) {
        users.push_back(it.key());
      }
    }

    const fs::path users_root = fs::path(storage_root_) / "users";
    if (fs::exists(users_root) && fs::is_directory(users_root)) {
      for (const auto &entry : fs::directory_iterator(users_root)) {
        if (entry.is_directory()) {
          users.push_back(entry.path().filename().string());
        }
      }
    }
  } catch (const fs::filesystem_error &) {
    // Failed to list super users
  }
  std::sort(users.begin(), users.end());
  users.erase(std::unique(users.begin(), users.end()), users.end());
  return users;
}

bool JsonStorageProvider::upsert_session_index_entry(const std::string &super_user,
                                                     const std::string &session_id,
                                                     const std::string &title) {
  std::scoped_lock lock(io_mutex_);
  try {
    json idx = load_index_unlocked();
    if (!idx["users"].contains(super_user) || !idx["users"][super_user].is_array()) {
      idx["users"][super_user] = json::array();
    }
    bool found = false;
    for (auto &entry : idx["users"][super_user]) {
      if (entry.is_object() && entry.value("session_id", "") == session_id) {
        entry["title"] = title;
        found = true;
        break;
      }
      if (entry.is_string() && entry.get<std::string>() == session_id) {
        entry = json{{"session_id", session_id}, {"title", title}};
        found = true;
        break;
      }
    }
    if (!found) {
      idx["users"][super_user].push_back(
          json{{"session_id", session_id}, {"title", title}});
    }
    return save_index_unlocked(idx);
  } catch (const std::exception &) {
    // Failed to upsert session index entry
    return false;
  }
}

bool JsonStorageProvider::delete_session_index_entry(const std::string &super_user,
                                                     const std::string &session_id) {
  std::scoped_lock lock(io_mutex_);
  try {
    json idx = load_index_unlocked();
    if (!idx["users"].contains(super_user) || !idx["users"][super_user].is_array()) {
      return false;
    }
    bool existed = false;
    json filtered = json::array();
    for (const auto &entry : idx["users"][super_user]) {
      std::string sid;
      if (entry.is_object()) {
        sid = entry.value("session_id", "");
      } else if (entry.is_string()) {
        sid = entry.get<std::string>();
      }
      if (sid == session_id) {
        existed = true;
        continue;
      }
      filtered.push_back(entry);
    }
    idx["users"][super_user] = filtered;
    save_index_unlocked(idx);
    return existed;
  } catch (const std::exception &) {
    // Failed to delete session index entry
    return false;
  }
}

std::vector<json>
JsonStorageProvider::list_session_index_entries(const std::string &super_user) {
  std::scoped_lock lock(io_mutex_);
  std::vector<json> out;
  try {
    json idx = load_index_unlocked();
    if (!idx["users"].contains(super_user) || !idx["users"][super_user].is_array()) {
      list_index_entries_from_fs(super_user, out);
      return out;
    }
    for (const auto &entry : idx["users"][super_user]) {
      if (entry.is_object()) {
        out.push_back(entry);
      } else if (entry.is_string()) {
        out.push_back(json{{"session_id", entry.get<std::string>()}, {"title", ""}});
      }
    }
    if (out.empty()) {
      list_index_entries_from_fs(super_user, out);
    }
  } catch (const fs::filesystem_error &) {
    // Failed to list session index entries
  }
  return out;
}

bool JsonStorageProvider::append_snapshot(const std::string &session_id,
                                          const json &snapshot) {
  std::scoped_lock lock(io_mutex_);
  try {
    int n = 1;
    while (fs::exists(snapshot_path(session_id, n))) {
      ++n;
    }
    const std::string path = snapshot_path(session_id, n);
    fs::create_directories(fs::path(path).parent_path());
    std::ofstream out(path);
    if (!out.is_open()) {
      return false;
    }
    out << snapshot.dump(2);
    return true;
  } catch (const std::exception &) {
    // Failed to append snapshot
    return false;
  }
}

int JsonStorageProvider::snapshot_count(const std::string &session_id) {
  std::scoped_lock lock(io_mutex_);
  int n = 0;
  while (fs::exists(snapshot_path(session_id, n + 1))) {
    ++n;
  }
  return n;
}

bool JsonStorageProvider::delete_snapshots(const std::string &session_id) {
  std::scoped_lock lock(io_mutex_);
  bool deleted = false;
  int n = 1;
  while (true) {
    const fs::path p(snapshot_path(session_id, n));
    if (!fs::exists(p)) {
      break;
    }
    fs::remove(p);
    deleted = true;
    ++n;
  }
  return deleted;
}

bool JsonStorageProvider::delete_snapshots_for_super_user(
    const std::string &super_user) {
  std::scoped_lock lock(io_mutex_);
  bool deleted = false;
  try {
    const fs::path user_root = fs::path(storage_root_) / "users" / super_user;
    if (!fs::exists(user_root) || !fs::is_directory(user_root)) {
      return false;
    }
    for (const auto &entry : fs::directory_iterator(user_root)) {
      if (!entry.is_directory()) {
        continue;
      }
      const std::string sid = entry.path().filename().string();
      delete_session_snapshots_internal(sid);
      deleted = true;
    }
  } catch (const fs::filesystem_error &) {
    // Failed to delete snapshots for super user
  }
  return deleted;
}

} // namespace velix::llm::storage
