#include <agentos/skills/skill_registry.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::skills;

class SkillRegistryTest : public ::testing::Test {
protected:
    SkillDef make_skill(const std::string& name,
                        std::vector<std::string> keywords,
                        std::string prompt = "") {
        SkillDef skill;
        skill.name = name;
        skill.description = "Test skill: " + name;
        skill.keywords = std::move(keywords);
        skill.prompt_injection = std::move(prompt);

        // Add a dummy tool
        tools::ToolSchema ts;
        ts.id = name + "_tool";
        ts.description = "Tool for " + name;
        skill.tools.push_back(ts);
        skill.tool_fns.push_back(
            [name](const tools::ParsedArgs&, std::stop_token) {
                return tools::ToolResult::ok(name + " executed");
            });
        return skill;
    }

    SkillRegistry registry_;
};

// ── Registration ──

TEST_F(SkillRegistryTest, RegisterAndCount) {
    EXPECT_EQ(registry_.skill_count(), 0u);
    registry_.register_skill(make_skill("search", {"search", "find", "query"}));
    EXPECT_EQ(registry_.skill_count(), 1u);
}

TEST_F(SkillRegistryTest, RemoveSkill) {
    registry_.register_skill(make_skill("search", {"search"}));
    registry_.remove_skill("search");
    EXPECT_EQ(registry_.skill_count(), 0u);
}

// ── Keyword matching ──

TEST_F(SkillRegistryTest, MatchesByKeyword) {
    registry_.register_skill(make_skill("search", {"search", "find", "query"}));
    registry_.register_skill(make_skill("deploy", {"deploy", "release", "ship"}));

    auto matches = registry_.match("I need to search for files");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].skill->name, "search");
    EXPECT_EQ(matches[0].hit_count, 1u);
}

TEST_F(SkillRegistryTest, MatchIsCaseInsensitive) {
    registry_.register_skill(make_skill("search", {"Search", "FIND"}));

    auto matches = registry_.match("Let me SEARCH and find stuff");
    ASSERT_EQ(matches.size(), 1u);
    EXPECT_EQ(matches[0].hit_count, 2u);
}

TEST_F(SkillRegistryTest, NoMatchReturnsEmpty) {
    registry_.register_skill(make_skill("search", {"search", "find"}));

    auto matches = registry_.match("deploy the application");
    EXPECT_TRUE(matches.empty());
}

TEST_F(SkillRegistryTest, MultipleMatchesSortedByHitCount) {
    registry_.register_skill(make_skill("search", {"search", "find", "query"}));
    registry_.register_skill(make_skill("analyze", {"search", "analyze", "report", "query"}));

    auto matches = registry_.match("search and query the data");
    ASSERT_EQ(matches.size(), 2u);
    // "analyze" has 2 hits (search, query), "search" has 2 hits (search, query)
    // Both have 2 hits — order among ties is unspecified
    EXPECT_GE(matches[0].hit_count, 2u);
}

// ── Activation / deactivation ──

TEST_F(SkillRegistryTest, ActivateRegistersTools) {
    registry_.register_skill(make_skill("search", {"search"}));
    tools::ToolRegistry tool_reg;

    auto res = registry_.activate("search", tool_reg, /*agent_id=*/1);
    ASSERT_TRUE(res.has_value());

    // Tool should now be findable
    auto tool = tool_reg.find("search_tool");
    EXPECT_NE(tool, nullptr);

    auto active = registry_.active_skills(1);
    ASSERT_EQ(active.size(), 1u);
    EXPECT_EQ(active[0], "search");
}

TEST_F(SkillRegistryTest, DeactivateUnregistersTools) {
    registry_.register_skill(make_skill("search", {"search"}));
    tools::ToolRegistry tool_reg;

    (void)registry_.activate("search", tool_reg, 1);
    auto res = registry_.deactivate("search", tool_reg, 1);
    ASSERT_TRUE(res.has_value());

    auto tool = tool_reg.find("search_tool");
    EXPECT_EQ(tool, nullptr);

    auto active = registry_.active_skills(1);
    EXPECT_TRUE(active.empty());
}

TEST_F(SkillRegistryTest, ActivateNonExistentReturnsNotFound) {
    tools::ToolRegistry tool_reg;
    auto res = registry_.activate("nonexistent", tool_reg, 1);
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ErrorCode::NotFound);
}

// ── Prompt injection ──

TEST_F(SkillRegistryTest, GetPromptReturnsInjection) {
    auto skill = make_skill("search", {"search"});
    skill.prompt_injection = "You have access to full-text search.";
    registry_.register_skill(std::move(skill));

    EXPECT_EQ(registry_.get_prompt("search"), "You have access to full-text search.");
}

TEST_F(SkillRegistryTest, GetPromptNonExistentReturnsEmpty) {
    EXPECT_EQ(registry_.get_prompt("nope"), "");
}

// ── Active skills per agent ──

TEST_F(SkillRegistryTest, ActiveSkillsPerAgent) {
    registry_.register_skill(make_skill("search", {"search"}));
    registry_.register_skill(make_skill("deploy", {"deploy"}));
    tools::ToolRegistry tool_reg;

    (void)registry_.activate("search", tool_reg, 1);
    (void)registry_.activate("deploy", tool_reg, 2);

    EXPECT_EQ(registry_.active_skills(1).size(), 1u);
    EXPECT_EQ(registry_.active_skills(2).size(), 1u);
    EXPECT_TRUE(registry_.active_skills(3).empty());
}
