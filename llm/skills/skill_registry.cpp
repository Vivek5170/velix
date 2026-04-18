#include "skill_registry.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <sstream>

namespace velix::llm::skills {

json SkillRegistry::get_skills_menu() const {
  json menu = json::array();
  for (const auto &[name, info] : cached_skills_) {
    if (info.metadata.visibility == "public" ||
        info.metadata.visibility == "internal") {
      menu.push_back({{"name", name},
                      {"description", info.metadata.description},
                      {"visibility", info.metadata.visibility}});
    }
  }
  return menu;
}

std::optional<SkillContent> SkillRegistry::get_skill(const std::string &skill_name,
                                                     size_t max_chars) {
  if (!is_valid_skill_name(skill_name)) {
    return std::nullopt;
  }

  // Check if full content is in cache and valid
  if (full_content_cache_.count(skill_name) && is_cache_valid(skill_name)) {
    auto content = full_content_cache_[skill_name];
    content.body = truncate_markdown(content.body, max_chars);
    return content;
  }

  // Otherwise, load fresh
  const std::filesystem::path repo_root = repo_root_from_cwd();
  const std::filesystem::path skill_file =
      repo_root / "skills" / skill_name / "SKILL.md";

  if (!std::filesystem::exists(skill_file)) {
    return std::nullopt;
  }

  // Read file
  std::ifstream in(skill_file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();

  if (content.size() > MAX_SKILL_SIZE_BYTES) {
    return std::nullopt;
  }

  // Parse front-matter and body
  auto [metadata, body] = parse_skill_frontmatter_and_body(content, skill_name);

  SkillContent result{metadata, truncate_markdown(body, max_chars)};

  // Cache the full content
  full_content_cache_[skill_name] = result;
  last_cache_time_ = std::chrono::system_clock::now();

  return result;
}

std::vector<std::string> SkillRegistry::list_skill_names() const {
  std::vector<std::string> names;
  for (const auto &[name, info] : cached_skills_) {
    (void)info;
    names.push_back(name);
  }
  return names;
}

bool SkillRegistry::skill_exists(const std::string &skill_name) const {
  return cached_skills_.count(skill_name) > 0;
}

void SkillRegistry::reload() {
  cached_skills_.clear();
  full_content_cache_.clear();
  load_from_skills_directory();
}

std::pair<SkillMetadata, std::string>
SkillRegistry::parse_skill_frontmatter_and_body(const std::string &content,
                                               const std::string &skill_name) {
  SkillMetadata metadata;
  metadata.name = skill_name;
  metadata.description = "";
  metadata.author = "";
  metadata.version = "";
  metadata.visibility = "public";

  std::string body = content;

  // Look for YAML front-matter delimiters (---\n ... \n---)
  if (content.substr(0, 3) != "---") {
    return {metadata, content};
  }

  size_t second_delimiter = content.find("\n---", 4);

  if (second_delimiter == std::string::npos) {
    return {metadata, content};
  }

  // Extract YAML block
  std::string yaml_block = content.substr(4, second_delimiter - 4);
  body = content.substr(second_delimiter + 5);

  // Parse YAML with support for both inline and list-based values
  std::istringstream yaml_stream(yaml_block);
  std::string line;
  std::string pending_key;  // For handling multi-line YAML lists

  while (std::getline(yaml_stream, line)) {
    // Trim whitespace
    size_t start = line.find_first_not_of(" \t\r\n");
    if (start != std::string::npos) {
      line = line.substr(start);
    } else {
      line.clear();
    }
    
    size_t end = line.find_last_not_of(" \t\r\n");
    if (end != std::string::npos) {
      line = line.substr(0, end + 1);
    } else {
      line.clear();
    }

    if (line.empty() || line[0] == '#') {
      pending_key.clear();
      continue;
    }

    // Handle YAML list items (lines starting with "- ")
    if (line[0] == '-' && (line.length() > 1 && line[1] == ' ')) {
      if (pending_key == "tags") {
        std::string tag = line.substr(2);  // Remove "- "
        // Trim tag
        tag.erase(0, tag.find_first_not_of(" \t\r\n"));
        tag.erase(tag.find_last_not_of(" \t\r\n") + 1);
        if (!tag.empty()) {
          metadata.tags.push_back(tag);
        }
      }
      continue;
    }

    size_t colon_pos = line.find(':');
    if (colon_pos == std::string::npos) {
      pending_key.clear();
      continue;
    }

    std::string key = line.substr(0, colon_pos);
    std::string value = line.substr(colon_pos + 1);

    // Trim key and value
    size_t key_end = key.find_last_not_of(" \t");
    if (key_end != std::string::npos) {
      key = key.substr(0, key_end + 1);
    } else {
      key.clear();
    }
    
    size_t value_start = value.find_first_not_of(" \t\r\n");
    if (value_start != std::string::npos) {
      value = value.substr(value_start);
    } else {
      value.clear();
    }
    
    size_t value_end = value.find_last_not_of(" \t\r\n");
    if (value_end != std::string::npos) {
      value = value.substr(0, value_end + 1);
    } else {
      value.clear();
    }

    // Remove quotes if present
    if (!value.empty() && value.length() >= 2 &&
        ((value.front() == '"' && value.back() == '"') ||
         (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.length() - 2);
    }

    if (key == "name") {
      metadata.name = value;
      pending_key.clear();
    } else if (key == "description") {
      metadata.description = value;
      pending_key.clear();
    } else if (key == "author") {
      metadata.author = value;
      pending_key.clear();
    } else if (key == "version") {
      metadata.version = value;
      pending_key.clear();
    } else if (key == "visibility") {
      metadata.visibility = value;
      pending_key.clear();
    } else if (key == "tags") {
      // tags can be inline (comma-separated) or list-based
      pending_key = "tags";
      if (!value.empty()) {
        // Inline format: tags: tag1, tag2, tag3
        std::istringstream tag_stream(value);
        std::string tag;
        while (std::getline(tag_stream, tag, ',')) {
          tag.erase(0, tag.find_first_not_of(" \t\r\n"));
          tag.erase(tag.find_last_not_of(" \t\r\n") + 1);
          if (!tag.empty()) {
            metadata.tags.push_back(tag);
          }
        }
        pending_key.clear();
      }
      // else: list-based format, will be handled in next iteration
    } else {
      pending_key.clear();
    }
  }

  return {metadata, body};
}

void SkillRegistry::load_from_skills_directory() {
  cached_skills_.clear();
  const std::filesystem::path repo_root = repo_root_from_cwd();
  const std::filesystem::path skills_root = repo_root / "skills";

  for (const auto &dir_name : list_directory_names(skills_root)) {
    if (auto skill_info = load_skill_file(repo_root, dir_name)) {
      cached_skills_[dir_name] = *skill_info;
    }
  }

  last_cache_time_ = std::chrono::system_clock::now();
}

std::optional<SkillInfo> SkillRegistry::load_skill_file(
    const std::filesystem::path &repo_root, const std::string &skill_name) {
  if (!is_valid_skill_name(skill_name)) {
    return std::nullopt;
  }

  const std::filesystem::path skill_file =
      repo_root / "skills" / skill_name / "SKILL.md";

  if (!std::filesystem::exists(skill_file)) {
    return std::nullopt;
  }

  // Read file
  std::ifstream in(skill_file, std::ios::binary);
  if (!in) {
    return std::nullopt;
  }

  std::string content((std::istreambuf_iterator<char>(in)),
                      std::istreambuf_iterator<char>());
  in.close();

  if (content.size() > MAX_SKILL_SIZE_BYTES) {
    return std::nullopt;
  }

  // Parse front-matter
  auto [metadata, body] = parse_skill_frontmatter_and_body(content, skill_name);

  // Extract preview (first 240 chars)
  std::string preview = body;
  if (preview.size() > 240) {
    preview = preview.substr(0, 240);
    size_t last_space = preview.find_last_of(" \t\n");
    if (last_space != std::string::npos) {
      preview = preview.substr(0, last_space);
    }
  }

  SkillInfo info{metadata, preview,
                 std::filesystem::last_write_time(skill_file)};
  return info;
}

bool SkillRegistry::is_cache_valid(const std::string &skill_name) {
  // Check TTL
  auto now = std::chrono::system_clock::now();
  auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
      now - last_cache_time_);
  if (elapsed.count() > CACHE_TTL_SECONDS) {
    return false;
  }

  // Check mtime
  if (cached_skills_.count(skill_name)) {
    const std::filesystem::path repo_root = repo_root_from_cwd();
    const std::filesystem::path skill_file =
        repo_root / "skills" / skill_name / "SKILL.md";

    if (std::filesystem::exists(skill_file)) {
      auto current_mtime = std::filesystem::last_write_time(skill_file);
      if (current_mtime != cached_skills_[skill_name].last_mtime) {
        return false;
      }
    }
  }

  return true;
}

std::string SkillRegistry::truncate_markdown(const std::string &text,
                                             size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }

  // Truncate at max_chars, preferring to end at a sentence or paragraph
  std::string truncated = text.substr(0, max_chars);

  // Find last period, newline, or word boundary
  size_t last_period = truncated.rfind('.');
  size_t last_newline = truncated.rfind('\n');
  size_t last_space = truncated.rfind(' ');

  size_t best_cut = last_period;
  if (last_newline != std::string::npos &&
      last_newline > last_period) {
    best_cut = last_newline;
  } else if (last_space != std::string::npos &&
             last_space > last_period) {
    best_cut = last_space;
  }

  if (best_cut != std::string::npos && best_cut > max_chars / 2) {
    truncated = truncated.substr(0, best_cut);
  }

  truncated += "\n...(truncated)";
  return truncated;
}

std::string SkillRegistry::normalize_name(const std::string &name) {
  std::string normalized;
  for (char c : name) {
    if (std::isalnum(c)) {
      normalized += std::tolower(c);
    } else if (c == '_' || c == '-' || c == ' ') {
      normalized += ' ';  // Normalize all delimiters to space
    }
  }
  // Trim and collapse multiple spaces
  std::string result;
  bool last_was_space = false;
  for (char c : normalized) {
    if (c == ' ') {
      if (!last_was_space) {
        result += ' ';
        last_was_space = true;
      }
    } else {
      result += c;
      last_was_space = false;
    }
  }
  // Trim leading/trailing spaces
  result.erase(0, result.find_first_not_of(" "));
  result.erase(result.find_last_not_of(" ") + 1);
  return result;
}

double SkillRegistry::levenshtein_score(const std::string &a,
                                       const std::string &b) {
  const size_t len_a = a.length();
  const size_t len_b = b.length();
  const size_t max_len = std::max(len_a, len_b);
  if (max_len == 0) return 1.0;

  // Simple Levenshtein distance (optimized for small strings)
  std::vector<std::vector<size_t>> dp(len_a + 1,
                                       std::vector<size_t>(len_b + 1, 0));
  for (size_t i = 0; i <= len_a; ++i) dp[i][0] = i;
  for (size_t j = 0; j <= len_b; ++j) dp[0][j] = j;

  for (size_t i = 1; i <= len_a; ++i) {
    for (size_t j = 1; j <= len_b; ++j) {
      size_t cost = (a[i - 1] == b[j - 1]) ? 0 : 1;
      dp[i][j] = std::min({dp[i - 1][j] + 1, dp[i][j - 1] + 1,
                           dp[i - 1][j - 1] + cost});
    }
  }

  double distance = dp[len_a][len_b];
  return 1.0 - (distance / max_len);
}

double SkillRegistry::token_overlap_score(const std::string &query,
                                          const std::string &target) {
  std::string norm_query = normalize_name(query);
  std::string norm_target = normalize_name(target);

  // Check for exact substring match (highest score)
  if (norm_target.find(norm_query) != std::string::npos) {
    return 0.95;
  }
  if (norm_query.find(norm_target) != std::string::npos) {
    return 0.90;
  }

  // Token-level overlap: count how many tokens overlap
  std::set<std::string> query_tokens;
  std::set<std::string> target_tokens;

  std::istringstream qs(norm_query);
  std::string token;
  while (qs >> token) {
    query_tokens.insert(token);
  }

  std::istringstream ts(norm_target);
  while (ts >> token) {
    target_tokens.insert(token);
  }

  if (query_tokens.empty() || target_tokens.empty()) {
    return 0.0;
  }

  size_t overlap = 0;
  for (const auto &qt : query_tokens) {
    if (target_tokens.count(qt)) {
      overlap++;
    }
  }

  return static_cast<double>(overlap) / query_tokens.size() * 0.8;
}

std::vector<SkillRegistry::SkillSuggestion> SkillRegistry::get_skill_suggestions(
    const std::string &query, double min_score, size_t max_candidates) const {
  std::vector<SkillSuggestion> suggestions;

  for (const auto &[name, info] : cached_skills_) {
    // Compute combined score: 0.6 token/prefix + 0.4 edit distance
    double token_score = token_overlap_score(query, name);
    double edit_score = levenshtein_score(normalize_name(query),
                                          normalize_name(name));

    double combined_score = 0.6 * token_score + 0.4 * edit_score;

    if (combined_score >= min_score) {
      suggestions.push_back({name, info.metadata.description, combined_score});
    }
  }

  // Sort by score descending
  std::sort(suggestions.begin(), suggestions.end(),
            [](const SkillSuggestion &a, const SkillSuggestion &b) {
              return a.score > b.score;
            });

  // Limit to max_candidates
  if (suggestions.size() > max_candidates) {
    suggestions.resize(max_candidates);
  }

  return suggestions;
}

}  // namespace velix::llm::skills
