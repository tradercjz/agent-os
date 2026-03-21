#include <agentos/agentos.hpp>
#include <agentos/core/agent_snapshot.hpp>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>

using namespace agentos;

TEST(AgentSnapshotTest, SerializeDeserializeRoundTrip) {
    AgentSnapshot snap;
    snap.agent_id = 42;
    snap.name = "test-agent";
    snap.role_prompt = "You are helpful";
    snap.security_role = "standard";
    snap.context_limit = 4096;
    snap.allowed_tools = {"kv_store", "shell_exec"};
    snap.messages = {{"system", "You are helpful"}, {"user", "Hello"}};
    snap.memory_entries_json = {R"({"id":"m1","content":"fact"})"};

    auto json = snap.to_json();
    auto restored = AgentSnapshot::from_json(json);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->agent_id, 42u);
    EXPECT_EQ(restored->name, "test-agent");
    EXPECT_EQ(restored->role_prompt, "You are helpful");
    EXPECT_EQ(restored->security_role, "standard");
    EXPECT_EQ(restored->context_limit, 4096u);
    EXPECT_EQ(restored->allowed_tools.size(), 2u);
    EXPECT_EQ(restored->messages.size(), 2u);
    EXPECT_EQ(restored->messages[0].first, "system");
    EXPECT_EQ(restored->messages[0].second, "You are helpful");
    EXPECT_EQ(restored->messages[1].first, "user");
    EXPECT_EQ(restored->messages[1].second, "Hello");
    EXPECT_EQ(restored->memory_entries_json.size(), 1u);
    EXPECT_EQ(restored->snapshot_version, "1.0");
}

TEST(AgentSnapshotTest, SaveAndLoadFile) {
    auto tmp = std::filesystem::temp_directory_path() / "test_agent_snapshot.json";
    AgentSnapshot snap;
    snap.agent_id = 1;
    snap.name = "file-test";
    snap.role_prompt = "test prompt";
    snap.context_limit = 2048;

    auto sr = snap.save(tmp.string());
    ASSERT_TRUE(sr.has_value());

    auto loaded = AgentSnapshot::load(tmp.string());
    ASSERT_TRUE(loaded.has_value());
    EXPECT_EQ(loaded->name, "file-test");
    EXPECT_EQ(loaded->role_prompt, "test prompt");
    EXPECT_EQ(loaded->context_limit, 2048u);

    std::filesystem::remove(tmp);
}

TEST(AgentSnapshotTest, LoadInvalidJsonReturnsError) {
    auto tmp = std::filesystem::temp_directory_path() / "bad_snapshot.json";
    { std::ofstream ofs(tmp); ofs << "not json{{{"; }
    auto r = AgentSnapshot::load(tmp.string());
    EXPECT_FALSE(r.has_value());
    std::filesystem::remove(tmp);
}

TEST(AgentSnapshotTest, LoadMissingFileReturnsError) {
    auto r = AgentSnapshot::load("/nonexistent/snapshot_xyz.json");
    EXPECT_FALSE(r.has_value());
}

TEST(AgentSnapshotTest, SnapshotAndRestoreAgent) {
    auto os = quickstart_mock();
    auto agent = make_agent(*os, "snap-test")
        .prompt("You are a snapshot test agent")
        .context(4096)
        .create();

    auto snap_result = os->snapshot_agent(agent->id());
    ASSERT_TRUE(snap_result.has_value());

    auto& snap = *snap_result;
    EXPECT_EQ(snap.name, "snap-test");
    EXPECT_EQ(snap.role_prompt, "You are a snapshot test agent");
    EXPECT_EQ(snap.context_limit, 4096u);

    // Save and reload from file
    auto tmp = std::filesystem::temp_directory_path() / "agent_snap_test.json";
    auto save_res = snap.save(tmp.string());
    ASSERT_TRUE(save_res.has_value());

    auto loaded = AgentSnapshot::load(tmp.string());
    ASSERT_TRUE(loaded.has_value());

    auto restored = os->restore_agent(*loaded);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ((*restored)->config().name, "snap-test");
    EXPECT_EQ((*restored)->config().role_prompt, "You are a snapshot test agent");
    EXPECT_EQ((*restored)->config().context_limit, 4096u);

    std::filesystem::remove(tmp);
}

TEST(AgentSnapshotTest, SnapshotNonexistentAgentReturnsError) {
    auto os = quickstart_mock();
    auto r = os->snapshot_agent(99999);
    EXPECT_FALSE(r.has_value());
}

TEST(AgentSnapshotTest, EmptySnapshotRoundTrip) {
    AgentSnapshot snap;
    auto json = snap.to_json();
    auto restored = AgentSnapshot::from_json(json);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->agent_id, 0u);
    EXPECT_TRUE(restored->name.empty());
    EXPECT_TRUE(restored->messages.empty());
    EXPECT_TRUE(restored->memory_entries_json.empty());
}
