/**
 * skill_view — Velix Tool
 *
 * Fetches and displays skill content from SKILL.md files.
 * Returns parsed JSON with metadata and body, with optional truncation.
 */

#include "../../runtime/sdk/cpp/velix_process.hpp"
#include "../../llm/skills/skill_registry.hpp"
#include "../../communication/json_include.hpp"
#include "../../utils/logger.hpp"

#include <iostream>
#include <string>
#include <sstream>

using json = nlohmann::json;
using namespace velix::core;

class SkillViewTool : public VelixProcess {
public:
  SkillViewTool() : VelixProcess("skill_view", "tool") {}

  void run() override {
    // Extract parameters
    if (!params.contains("skill_name") || !params["skill_name"].is_string()) {
      json error_response = {
          {"status", "error"},
          {"message", "Missing required parameter: skill_name"}};
      report_result(parent_pid, error_response, entry_trace_id);
      return;
    }

    std::string skill_name = params["skill_name"].get<std::string>();
    std::string mode = params.value("mode", std::string("summary"));
    size_t max_chars = params.value("max_chars", size_t(8000));

    // Validate and clamp max_chars
    if (max_chars < 100 || max_chars > 100000) {
      max_chars = 8000;
    }

    // Validate mode
    if (mode != "summary" && mode != "manifest" && mode != "body") {
      mode = "summary";
    }

    // Initialize SkillRegistry and fetch skill
    velix::llm::skills::SkillRegistry registry;

    auto skill_opt = registry.get_skill(skill_name, max_chars);
    if (!skill_opt) {
      // Skill not found; try fuzzy-matching to suggest alternatives
      auto suggestions = registry.get_skill_suggestions(skill_name, 0.55, 3);
      
      // Audit log the fuzzy-match request
      std::ostringstream audit_msg;
      audit_msg << "Fuzzy-match attempt: query=\"" << skill_name 
                << "\" suggestions=" << suggestions.size();
      velix::utils::Logger::info(audit_msg.str(), 
          velix::utils::LogContext{"skill_view", entry_trace_id, parent_pid, "fuzzy_match"});
      
      json error_response = {
          {"status", "error"},
          {"message", "Skill not found: " + skill_name}};
      
      if (!suggestions.empty()) {
        json suggestions_array = json::array();
        for (const auto &sugg : suggestions) {
          suggestions_array.push_back({
              {"name", sugg.name},
              {"description", sugg.description},
              {"score", sugg.score}
          });
        }
        error_response["suggestions"] = suggestions_array;
      }
      
      report_result(parent_pid, error_response, entry_trace_id);
      return;
    }

    auto skill = *skill_opt;

    // Build response based on mode
    json response = {{"status", "ok"}};

    json skill_json;
    skill_json["name"] = skill.metadata.name;
    skill_json["description"] = skill.metadata.description;
    skill_json["author"] = skill.metadata.author;
    skill_json["version"] = skill.metadata.version;
    skill_json["visibility"] = skill.metadata.visibility;
    skill_json["tags"] = skill.metadata.tags;

    if (mode == "manifest") {
      response["skill"] = skill_json;
    } else if (mode == "body") {
      response["body"] = skill.body;
    } else {  // summary (default)
      skill_json["body"] = skill.body;
      response["skill"] = skill_json;
    }

    report_result(parent_pid, response, entry_trace_id);
  }
};

int main() {
  auto tool = std::make_unique<SkillViewTool>();
  tool->start();
  return 0;
}
