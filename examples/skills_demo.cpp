// ============================================================
// AgentOS Skill Registry Demo
// 演示 Skill 的注册、关键词匹配、自动激活与去激活
// ============================================================
#include <agentos/agentos.hpp>
#include <agentos/skills/skill_registry.hpp>
#include <iostream>

using namespace agentos;
using namespace agentos::skills;

void ok(std::string_view msg) {
  std::cout << "\033[32m  ✓ " << msg << "\033[0m\n";
}
void info(std::string_view msg) {
  std::cout << "\033[34m  · " << msg << "\033[0m\n";
}
void section(std::string_view title) {
  std::cout << "\n\033[1;36m══════════════════════════════════════\033[0m\n  "
            << "\033[1;33m" << title << "\033[0m\n"
            << "\033[1;36m══════════════════════════════════════\033[0m\n";
}

int main() {
    SkillRegistry registry;

    // ── 1. 注册 Skills ──
    section("1. 注册 Skills");

    // 搜索技能
    SkillDef search_skill;
    search_skill.name = "web-search";
    search_skill.description = "Web search capability";
    search_skill.keywords = {"search", "find", "look up", "query", "google"};
    search_skill.prompt_injection = "You have web search capability. "
                                    "Use the search tool to find information online.";
    search_skill.tools.push_back(
        tools::ToolSchema{.id = "web_search", .description = "搜索互联网"});
    search_skill.tool_fns.push_back(
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string q = args.values.count("query")
                ? args.values.at("query") : "default";
            return tools::ToolResult::ok("Results for: " + q);
        });
    registry.register_skill(std::move(search_skill));
    info("注册: web-search (关键词: search, find, look up, query, google)");

    // 数据库技能
    SkillDef db_skill;
    db_skill.name = "database";
    db_skill.description = "Database query and management";
    db_skill.keywords = {"database", "sql", "query", "table", "select"};
    db_skill.prompt_injection = "You can query databases using SQL.";
    db_skill.tools.push_back(
        tools::ToolSchema{.id = "sql_query", .description = "执行 SQL 查询"});
    db_skill.tool_fns.push_back(
        [](const tools::ParsedArgs& args, std::stop_token) {
            std::string sql = args.values.count("sql")
                ? args.values.at("sql") : "SELECT 1";
            return tools::ToolResult::ok("Query result: [" + sql + "] → 42 rows");
        });
    registry.register_skill(std::move(db_skill));
    info("注册: database (关键词: database, sql, query, table, select)");

    // 部署技能
    SkillDef deploy_skill;
    deploy_skill.name = "deployment";
    deploy_skill.description = "Application deployment";
    deploy_skill.keywords = {"deploy", "release", "ship", "production", "staging"};
    deploy_skill.prompt_injection = "You can deploy applications to staging or production.";
    deploy_skill.tools.push_back(
        tools::ToolSchema{.id = "deploy_app", .description = "部署应用"});
    deploy_skill.tool_fns.push_back(
        [](const tools::ParsedArgs&, std::stop_token) {
            return tools::ToolResult::ok("Deployed successfully");
        });
    registry.register_skill(std::move(deploy_skill));
    info("注册: deployment (关键词: deploy, release, ship, production, staging)");

    info(("已注册 " + std::to_string(registry.skill_count()) + " 个 Skills").c_str());
    ok("Skills 注册完成");

    // ── 2. 关键词匹配 ──
    section("2. 关键词匹配 (match)");

    auto test_match = [&](const std::string& task) {
        info(("任务: \"" + task + "\"").c_str());
        auto matches = registry.match(task);
        if (matches.empty()) {
            info("  → 无匹配");
        } else {
            for (const auto& m : matches) {
                info(("  → " + m.skill->name + " (命中 "
                      + std::to_string(m.hit_count) + " 个关键词)").c_str());
            }
        }
    };

    test_match("Help me search for the latest news");
    test_match("Query the database for user records");
    test_match("Search the database and query results");  // 匹配两个 skill
    test_match("Deploy the app to production");
    test_match("Write a poem about cats");  // 无匹配
    ok("关键词匹配测试完成");

    // ── 3. 激活 Skill ──
    section("3. 激活 Skill (activate)");

    tools::ToolRegistry tool_reg;
    AgentId agent_id = 1;

    // 根据任务自动激活匹配的 skill
    std::string task = "Search the database for recent orders";
    info(("任务: \"" + task + "\"").c_str());

    auto matches = registry.match(task);
    for (const auto& m : matches) {
        auto res = registry.activate(m.skill->name, tool_reg, agent_id);
        if (res) {
            info(("激活: " + m.skill->name).c_str());
            auto prompt = registry.get_prompt(m.skill->name);
            if (!prompt.empty()) {
                info(("  注入 prompt: \"" + prompt.substr(0, 50) + "...\"").c_str());
            }
        }
    }

    auto active = registry.active_skills(agent_id);
    info(("当前活跃 Skills: " + std::to_string(active.size()) + " 个").c_str());
    for (const auto& name : active) {
        info(("  - " + name).c_str());
    }
    ok("Skills 已自动激活");

    // ── 4. 使用激活的工具 ──
    section("4. 使用激活的工具");

    auto search_tool = tool_reg.find("web_search");
    if (search_tool) {
        tools::ParsedArgs args;
        args.values["query"] = "recent orders";
        auto result = search_tool->execute(args);
        info(("web_search → " + result.output).c_str());
    }

    auto sql_tool = tool_reg.find("sql_query");
    if (sql_tool) {
        tools::ParsedArgs args;
        args.values["sql"] = "SELECT * FROM orders WHERE date > '2026-03-01'";
        auto result = sql_tool->execute(args);
        info(("sql_query → " + result.output).c_str());
    }

    // deploy_app 不应该存在（没有被激活）
    auto deploy_tool = tool_reg.find("deploy_app");
    info(("deploy_app 是否可用: " + std::string(deploy_tool ? "是" : "否")).c_str());
    ok("只有匹配的工具被注册");

    // ── 5. 去激活 ──
    section("5. 去激活 Skill (deactivate)");

    for (const auto& name : registry.active_skills(agent_id)) {
        (void)registry.deactivate(name, tool_reg, agent_id);
        info(("去激活: " + name).c_str());
    }

    info(("活跃 Skills: " + std::to_string(registry.active_skills(agent_id).size())).c_str());
    info(("web_search 是否可用: "
          + std::string(tool_reg.find("web_search") ? "是" : "否")).c_str());
    ok("所有 Skills 已去激活，工具已移除");

    std::cout << "\n\033[1;32m🎉 Skill Registry Demo 完成！\033[0m\n\n";
    return 0;
}
