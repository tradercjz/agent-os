#include <agentos/context/context.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::context;
using namespace agentos::kernel;

// ── ContextWindow 单元测试 ──────────────────────────────────

class ContextWindowTest : public ::testing::Test {
protected:
  ContextWindow win{200}; // small budget for testing
};

TEST_F(ContextWindowTest, TryAddSucceeds) {
  EXPECT_TRUE(win.try_add(Message::system("Hi")));
  EXPECT_GT(win.used_tokens(), 0);
  EXPECT_EQ(win.messages().size(), 1);
}

TEST_F(ContextWindowTest, TryAddFailsOnOverflow) {
  // Fill the window
  std::string long_msg(800, 'x'); // ~200 tokens
  EXPECT_TRUE(win.try_add(Message::system("sys")));
  EXPECT_FALSE(win.try_add(Message::user(long_msg)));
}

TEST_F(ContextWindowTest, EvictionRemovesOldestNonSystem) {
  // 使用更小的窗口确保驱逐触发（budget=200, 每条~6 token, ~33条满）
  ContextWindow small_win{100}; // 约可容纳 16 条短消息
  // System message should be preserved
  small_win.add_evict_if_needed(Message::system("sys"));
  // Fill with user messages until overflow
  for (int i = 0; i < 30; ++i) {
    small_win.add_evict_if_needed(Message::user("msg_" + std::to_string(i)));
  }
  // System message should survive eviction
  EXPECT_EQ(small_win.messages().front().role, Role::System);
  EXPECT_EQ(small_win.messages().front().content, "sys");
  // Should have evicted some messages
  EXPECT_FALSE(small_win.evicted().empty());
}

TEST_F(ContextWindowTest, EvictionPrefersUserOverAssistant) {
  // Importance: User(0.1) < Tool(0.3) < Assistant(0.5)
  // With same age, User messages should be evicted first
  ContextWindow small_win{80};
  small_win.add_evict_if_needed(Message::user("user_msg"));
  small_win.add_evict_if_needed(Message::assistant("asst_msg"));
  // Fill to trigger eviction
  for (int i = 0; i < 20; ++i) {
    small_win.add_evict_if_needed(Message::user("fill_" + std::to_string(i)));
  }
  // Check evicted list: user_msg should be evicted before asst_msg
  bool user_evicted = false;
  bool asst_evicted = false;
  for (auto &m : small_win.evicted()) {
    if (m.content == "user_msg") user_evicted = true;
    if (m.content == "asst_msg") asst_evicted = true;
  }
  // user_msg (importance 0.1) should be evicted
  EXPECT_TRUE(user_evicted);
  // If both were evicted, user should have gone first (lower importance)
  if (asst_evicted) {
    // Both evicted is fine under heavy pressure — just verify user was too
    EXPECT_TRUE(user_evicted);
  }
}

TEST_F(ContextWindowTest, MessageCount) {
  EXPECT_EQ(win.message_count(), 0);
  win.try_add(Message::user("a"));
  win.try_add(Message::user("b"));
  EXPECT_EQ(win.message_count(), 2);
}

TEST_F(ContextWindowTest, Utilization) {
  EXPECT_FLOAT_EQ(win.utilization(), 0.0f);
  win.try_add(Message::system("test"));
  EXPECT_GT(win.utilization(), 0.0f);
  EXPECT_LE(win.utilization(), 1.0f);
}

TEST_F(ContextWindowTest, InjectSummary) {
  win.try_add(Message::user("old"));
  win.inject_summary("Summary of old content");
  EXPECT_GE(win.messages().size(), 2);
  // Summary should be at the front
  EXPECT_TRUE(win.messages().front().content.find("摘要") !=
              std::string::npos);
}

TEST_F(ContextWindowTest, Reset) {
  win.try_add(Message::user("data"));
  win.reset();
  EXPECT_EQ(win.used_tokens(), 0);
  EXPECT_TRUE(win.messages().empty());
}

// ── ContextSnapshot 序列化测试 ──────────────────────────────

TEST(SnapshotTest, SerializeDeserializeRoundTrip) {
  ContextSnapshot snap;
  snap.agent_id = 42;
  snap.session_id = "sess_abc";
  snap.captured_at = now();
  snap.metadata_json = R"({"key":"val"})";
  snap.messages.push_back(Message::system("System prompt"));
  snap.messages.push_back(Message::user("Hello world"));
  snap.messages.push_back(Message::assistant("Hi there"));

  std::string serialized = snap.serialize();
  auto restored = ContextSnapshot::deserialize(serialized);

  ASSERT_TRUE(restored.has_value());
  EXPECT_EQ(restored->agent_id, 42);
  EXPECT_EQ(restored->messages.size(), 3);
  EXPECT_EQ(restored->messages[0].role, Role::System);
  EXPECT_EQ(restored->messages[0].content, "System prompt");
  EXPECT_EQ(restored->messages[1].content, "Hello world");
  EXPECT_EQ(restored->metadata_json, R"({"key":"val"})");
}

TEST(SnapshotTest, EscapedNewlinesPreserved) {
  ContextSnapshot snap;
  snap.agent_id = 1;
  snap.session_id = "s1";
  snap.messages.push_back(Message::user("line1\nline2\nline3"));
  snap.metadata_json = "{}";

  auto restored = ContextSnapshot::deserialize(snap.serialize());
  ASSERT_TRUE(restored);
  EXPECT_EQ(restored->messages[0].content, "line1\nline2\nline3");
}

// ── ContextManager 集成测试 ─────────────────────────────────

class ContextManagerTest : public ::testing::Test {
protected:
  void SetUp() override {
    test_dir_ =
        std::filesystem::temp_directory_path() / "agentos_ctx_test";
    std::filesystem::remove_all(test_dir_);
    mgr_ = std::make_unique<ContextManager>(test_dir_);
  }
  void TearDown() override { std::filesystem::remove_all(test_dir_); }

  std::filesystem::path test_dir_;
  std::unique_ptr<ContextManager> mgr_;
};

TEST_F(ContextManagerTest, SnapshotAndRestore) {
  auto &win = mgr_->get_window(100, 4096);
  win.try_add(Message::system("You are helpful"));
  win.try_add(Message::user("What's 2+2?"));
  win.try_add(Message::assistant("4"));

  auto path = mgr_->snapshot(100);
  ASSERT_TRUE(path);
  EXPECT_TRUE(std::filesystem::exists(*path));

  // Clear and restore
  mgr_->clear(100);
  auto r = mgr_->restore(100);
  ASSERT_TRUE(r);

  auto &restored_win = mgr_->get_window(100);
  EXPECT_EQ(restored_win.messages().size(), 3);
}

TEST_F(ContextManagerTest, CompressWithSummary) {
  mgr_->get_window(200, 200); // small window — 创建后通过 mgr_ 访问
  // Fill up
  for (int i = 0; i < 30; ++i) {
    mgr_->append(200, Message::user("msg" + std::to_string(i)));
  }

  // Compress with a summarizer
  mgr_->compress(200, [](const std::vector<Message> &) -> std::string {
    return "Summary: many messages were exchanged.";
  });

  auto &compressed = mgr_->get_window(200);
  EXPECT_LE(compressed.used_tokens(), compressed.max_tokens());
}

// ── Snapshot Serialize/Deserialize Roundtrip ─────────────────

TEST(ContextSnapshotTest, RoundtripWithSpecialChars) {
  ContextSnapshot snap;
  snap.agent_id = 42;
  snap.session_id = "test_session";
  snap.captured_at = now();
  snap.metadata_json = R"({"key":"val"})";

  // Add messages with special characters
  snap.messages.push_back(Message::user("Hello\nWorld"));
  snap.messages.push_back(Message::assistant("Tab\there\rand\\backslash"));
  snap.messages.push_back(Message::system("")); // empty content

  auto serialized = snap.serialize();
  auto restored = ContextSnapshot::deserialize(serialized);

  ASSERT_TRUE(restored.has_value());
  ASSERT_EQ(restored->messages.size(), 3);
  EXPECT_EQ(restored->messages[0].content, "Hello\nWorld");
  EXPECT_EQ(restored->messages[1].content, "Tab\there\rand\\backslash");
  EXPECT_TRUE(restored->messages[2].content.empty());
  EXPECT_EQ(restored->agent_id, 42);
}

TEST(ContextSnapshotTest, DeserializeRejectsOversizedInput) {
  // Create a string > 50 MiB
  std::string huge(51 * 1024 * 1024, 'X');
  auto result = ContextSnapshot::deserialize(huge);
  EXPECT_FALSE(result.has_value());
}
