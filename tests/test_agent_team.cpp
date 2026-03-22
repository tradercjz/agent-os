#include <agentos/core/agent_team.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(AgentTeamTest, AddRemoveMembers) {
    AgentTeam team("alpha");
    EXPECT_TRUE(team.add_member(1));
    EXPECT_TRUE(team.add_member(2));
    EXPECT_TRUE(team.has_member(1));
    EXPECT_EQ(team.size(), 2u);
    EXPECT_TRUE(team.remove_member(1));
    EXPECT_FALSE(team.has_member(1));
}

TEST(AgentTeamTest, MaxMembersEnforced) {
    AgentTeam team("small", {.max_members = 2});
    EXPECT_TRUE(team.add_member(1));
    EXPECT_TRUE(team.add_member(2));
    EXPECT_FALSE(team.add_member(3)); // full
}

TEST(AgentTeamTest, DuplicateAddReturnsFalse) {
    AgentTeam team("t");
    EXPECT_TRUE(team.add_member(1));
    EXPECT_FALSE(team.add_member(1)); // duplicate
}

TEST(TeamManagerTest, CreateAndFind) {
    TeamManager mgr;
    auto r = mgr.create_team("team-a", {.description = "Alpha team"});
    ASSERT_TRUE(r.has_value());
    auto* t = mgr.find_team("team-a");
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->name(), "team-a");
}

TEST(TeamManagerTest, DuplicateCreateFails) {
    TeamManager mgr;
    (void)mgr.create_team("dup");
    auto r = mgr.create_team("dup");
    EXPECT_FALSE(r.has_value());
}

TEST(TeamManagerTest, RemoveTeam) {
    TeamManager mgr;
    (void)mgr.create_team("temp");
    EXPECT_EQ(mgr.team_count(), 1u);
    (void)mgr.remove_team("temp");
    EXPECT_EQ(mgr.team_count(), 0u);
}

TEST(TeamManagerTest, AssignAndTeamsOf) {
    TeamManager mgr;
    (void)mgr.create_team("t1");
    (void)mgr.create_team("t2");
    (void)mgr.assign(42, "t1");
    (void)mgr.assign(42, "t2");
    auto teams = mgr.teams_of(42);
    EXPECT_EQ(teams.size(), 2u);
}

TEST(TeamManagerTest, ListTeams) {
    TeamManager mgr;
    (void)mgr.create_team("alpha");
    (void)mgr.create_team("beta");
    auto names = mgr.list_teams();
    EXPECT_EQ(names.size(), 2u);
}

TEST(TeamManagerTest, AssignToNonexistentFails) {
    TeamManager mgr;
    auto r = mgr.assign(1, "nope");
    EXPECT_FALSE(r.has_value());
}
