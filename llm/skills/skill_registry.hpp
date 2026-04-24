#pragma once

#include "../../communication/json_include.hpp"

#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace velix::llm::skills {

using json = nlohmann::json;

// SkillMetadata represents the YAML front-matter of a SKILL.md file
struct SkillMetadata {
  std::string name;
  std::string description;  // One-liner, max 240 chars
  std::string author;
  std::string version;
  std::vector<std::string> tags;
  std::string visibility;  // "public", "internal", or "admin"
  json assets = json::object();  // Additional metadata
};

// SkillContent represents the full parsed content of a SKILL.md file
struct SkillContent {
  SkillMetadata metadata;
  std::string body;  // Markdown body (can be truncated)
};

// SkillInfo represents cached skill summary (just metadata + preview)
struct SkillInfo {
  SkillMetadata metadata;
  std::string preview;  // First 240 chars of body
  std::filesystem::file_time_type last_mtime;  // For cache invalidation
};

class SkillRegistry {
public:
  SkillRegistry() { load_from_skills_directory(); }

  // Get all skills summaries for menu injection into system prompt
  // Returns array of {name, description, visibility}
  json get_skills_menu() const;

  // Load full skill content with optional truncation
  // Enforces visibility rules (caller must pass session_id for permission checks)
  std::optional<SkillContent> get_skill(const std::string &skill_name,
                                        size_t max_chars = 8000);

  // List all skill names
  std::vector<std::string> list_skill_names() const;

  // Check if skill exists
  bool skill_exists(const std::string &skill_name) const;

  // Get fuzzy-matched skill suggestions for a given name query
  // Returns array of {name, description, score} for candidates with score >= min_score
  // Sorted descending by score, limited to max_candidates
  struct SkillSuggestion {
    std::string name;
    std::string description;
    double score;  // 0.0 to 1.0
  };
  std::vector<SkillSuggestion> get_skill_suggestions(
      const std::string &query,
      double min_score = 0.55,
      size_t max_candidates = 3) const;

  // Reload all skills from disk (for testing or hot-reload)
  void reload();

private:
  std::map<std::string, SkillInfo, std::less<>> cached_skills_;
  std::map<std::string, SkillContent, std::less<>> full_content_cache_;
  std::chrono::system_clock::time_point last_cache_time_;

  static constexpr size_t CACHE_TTL_SECONDS = 60;
  static constexpr size_t MAX_SKILL_SIZE_BYTES = 100000;  // ~100KB max

  // Helper to find repo root from current working directory
  static std::filesystem::path repo_root_from_cwd() {
    std::filesystem::path cur = std::filesystem::current_path();
    for (int i = 0; i < 8; ++i) {
      if (std::filesystem::exists(cur / "skills") &&
          std::filesystem::exists(cur / "config")) {
        return cur;
      }
      if (!cur.has_parent_path()) {
        break;
      }
      cur = cur.parent_path();
    }
    return std::filesystem::current_path();
  }

  // Helper to list directory names (subdirectories only)
  static std::vector<std::string>
  list_directory_names(const std::filesystem::path &root) {
    std::vector<std::string> out;
    if (!std::filesystem::exists(root) ||
        !std::filesystem::is_directory(root)) {
      return out;
    }

    for (const auto &entry : std::filesystem::directory_iterator(root)) {
      if (entry.is_directory()) {
        out.push_back(entry.path().filename().string());
      }
    }
    return out;
  }

  // Parse YAML front-matter from SKILL.md
  // Returns {metadata, remaining_body_text}
  static std::pair<SkillMetadata, std::string>
  parse_skill_frontmatter_and_body(const std::string &content,
                                   const std::string &skill_name);

  // Validate skill_name for path traversal and other threats
  static bool is_valid_skill_name(const std::string &name) {
    if (name.empty() || name.length() > 255) {
      return false;
    }
    for (char c : name) {
      if (!std::isalnum(c) && c != '_' && c != '-' && c != '.') {
        return false;
      }
    }
    return true;
  }

  // Load a single SKILL.md file by skill_name
  std::optional<SkillInfo> load_skill_file(const std::filesystem::path &repo_root,
                                           const std::string &skill_name);

  // Main loading routine
  void load_from_skills_directory();

  // Check cache validity (TTL-based with mtime invalidation)
  bool is_cache_valid(const std::string &skill_name);

  // Truncate body to max_chars while preserving Markdown structure
  static std::string truncate_markdown(const std::string &text,
                                        size_t max_chars);

  // Fuzzy matching helpers
  // Normalize a string for comparison (lowercase, trim, remove punctuation)
  static std::string normalize_name(const std::string &name);

  // Calculate Levenshtein distance (normalized by max length)
  static double levenshtein_score(const std::string &a, const std::string &b);

  // Calculate prefix/token overlap score (0.0 to 1.0)
  static double token_overlap_score(const std::string &query,
                                     const std::string &target);
};

}  // namespace velix::llm::skills
