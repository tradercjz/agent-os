// ============================================================
// Extended tests for Context Manager
// Covers: add/count, token counting, overflow/compaction, system message
// preservation, serialization, clear, multi-agent windows, tool call
// messages, ordering, session save/load roundtrip
// ============================================================
#include <agentos/context/context.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <string>
#include <vector>

using namespace agentos;
using namespace agentos::context;
using namespace agentos::kernel;

namespace fs = std::filesystem;

static fs::path make_ctx_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("agentos_ctxext_" + name + "_" + std::to_string(nonce));
}

// ─────────────────────────────────────────────────────────────
// 1. Context window add and count messages
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, AddAndCountMessages) {
  ContextWindow win(4096);

  EXPECT_EQ(win.message_count(), 0u);
  EXPECT_EQ(win.used_tokens(), 0u);

  EXPECT_TRUE(win.try_add(Message::system("You are a helpful assistant.")));
  EXPECT_EQ(win.message_count(), 1u);

  EXPECT_TRUE(win.try_add(Message::user("Hello")));
  EXPECT_EQ(win.message_count(), 2u);

  EXPECT_TRUE(win.try_add(Message::assistant("Hi there!")));
  EXPECT_EQ(win.message_count(), 3u);

  // Verify messages are accessible
  const auto &msgs = win.messages();
  EXPECT_EQ(msgs.size(), 3u);
  EXPECT_EQ(msgs[0].role, Role::System);
  EXPECT_EQ(msgs[1].role, Role::User);
  EXPECT_EQ(msgs[2].role, Role::Assistant);
}

// ─────────────────────────────────────────────────────────────
// 2. Context window token counting
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, TokenCounting) {
  ContextWindow win(8192);

  TokenCount before = win.used_tokens();
  EXPECT_EQ(before, 0u);

  win.try_add(Message::user("Hello world, this is a test message."));
  TokenCount after = win.used_tokens();
  EXPECT_GT(after, 0u);

  // Adding more messages increases token count
  win.try_add(Message::assistant("Greetings! How can I help you today?"));
  EXPECT_GT(win.used_tokens(), after);

  // Utilization should be between 0 and 1
  EXPECT_GT(win.utilization(), 0.0f);
  EXPECT_LE(win.utilization(), 1.0f);
}

// ─────────────────────────────────────────────────────────────
// 3. Context window overflow triggers compaction (eviction)
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, OverflowTriggersEviction) {
  // Small window to trigger eviction quickly
  ContextWindow win(80);

  // Fill with messages until overflow
  for (int i = 0; i < 30; ++i) {
    win.add_evict_if_needed(Message::user("message_" + std::to_string(i)));
  }

  // Token usage should be within budget
  EXPECT_LE(win.used_tokens(), win.max_tokens());

  // Some messages should have been evicted
  EXPECT_FALSE(win.evicted().empty());
  EXPECT_LT(win.message_count(), 30u);
}

// ─────────────────────────────────────────────────────────────
// 4. Context window with system message preservation
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, SystemMessagePreservedDuringEviction) {
  ContextWindow win(100);

  // Add system message first
  win.add_evict_if_needed(Message::system("You are a helpful bot."));

  // Fill with user messages to force eviction
  for (int i = 0; i < 40; ++i) {
    win.add_evict_if_needed(Message::user("fill_msg_" + std::to_string(i)));
  }

  // System message should still be the first message
  EXPECT_EQ(win.messages().front().role, Role::System);
  EXPECT_EQ(win.messages().front().content, "You are a helpful bot.");

  // No system message should appear in evicted list
  for (const auto &m : win.evicted()) {
    EXPECT_NE(m.role, Role::System);
  }
}

// ─────────────────────────────────────────────────────────────
// 5. Context serialization and deserialization
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, SnapshotSerializationRoundtrip) {
  ContextSnapshot snap;
  snap.agent_id = 100;
  snap.session_id = "session_alpha";
  snap.captured_at = now();
  snap.metadata_json = R"({"model":"gpt-4","temp":0.7})";

  snap.messages.push_back(Message::system("System prompt here"));
  snap.messages.push_back(Message::user("What is 2+2?"));
  snap.messages.push_back(Message::assistant("4"));
  snap.messages.push_back(Message::user("Thanks!"));

  auto serialized = snap.serialize_binary();
  ASSERT_FALSE(serialized.empty());

  auto restored = ContextSnapshot::deserialize_binary(serialized);
  ASSERT_TRUE(restored.has_value());

  EXPECT_EQ(restored->agent_id, 100u);
  EXPECT_EQ(restored->session_id, "session_alpha");
  EXPECT_EQ(restored->metadata_json, R"({"model":"gpt-4","temp":0.7})");
  ASSERT_EQ(restored->messages.size(), 4u);
  EXPECT_EQ(restored->messages[0].content, "System prompt here");
  EXPECT_EQ(restored->messages[1].content, "What is 2+2?");
  EXPECT_EQ(restored->messages[2].content, "4");
  EXPECT_EQ(restored->messages[3].content, "Thanks!");
}

TEST(ContextExtended, SessionStateSerializationRoundtrip) {
  SessionState state;
  state.agent_id = 55;
  state.session_id = "sess_roundtrip";
  state.saved_at = now();
  state.config_json = R"({"name":"test_agent"})";
  state.middleware_names = {"auth_mw", "logging_mw"};
  state.metadata_json = R"({"version":2})";

  state.context.agent_id = 55;
  state.context.session_id = "sess_roundtrip";
  state.context.captured_at = now();
  state.context.metadata_json = "{}";
  state.context.messages.push_back(Message::system("sys"));
  state.context.messages.push_back(Message::user("input"));

  auto serialized = state.serialize_binary();
  ASSERT_FALSE(serialized.empty());

  auto restored = SessionState::deserialize_binary(serialized);
  ASSERT_TRUE(restored.has_value());

  EXPECT_EQ(restored->agent_id, 55u);
  EXPECT_EQ(restored->session_id, "sess_roundtrip");
  EXPECT_EQ(restored->config_json, R"({"name":"test_agent"})");
  ASSERT_EQ(restored->middleware_names.size(), 2u);
  EXPECT_EQ(restored->middleware_names[0], "auth_mw");
  EXPECT_EQ(restored->middleware_names[1], "logging_mw");
  ASSERT_EQ(restored->context.messages.size(), 2u);
  EXPECT_EQ(restored->context.messages[0].role, Role::System);
  EXPECT_EQ(restored->context.messages[1].role, Role::User);
}

// ─────────────────────────────────────────────────────────────
// 6. Context clear removes all messages
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, ClearRemovesAllMessages) {
  ContextWindow win(4096);

  win.try_add(Message::system("prompt"));
  win.try_add(Message::user("hello"));
  win.try_add(Message::assistant("hi"));

  EXPECT_EQ(win.message_count(), 3u);
  EXPECT_GT(win.used_tokens(), 0u);

  win.reset();

  EXPECT_EQ(win.message_count(), 0u);
  EXPECT_EQ(win.used_tokens(), 0u);
  EXPECT_TRUE(win.messages().empty());
  EXPECT_TRUE(win.evicted().empty());
}

TEST(ContextExtended, ContextManagerClearRemovesWindow) {
  auto dir = make_ctx_test_dir("mgr_clear");
  ContextManager mgr(dir);

  auto &win = mgr.get_window(42, 4096);
  win.try_add(Message::user("data"));
  EXPECT_EQ(win.message_count(), 1u);

  mgr.clear(42);

  // Getting the window again should create a fresh one
  auto &win2 = mgr.get_window(42, 4096);
  EXPECT_EQ(win2.message_count(), 0u);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 7. Multiple context windows for different agents
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, MultipleAgentWindows) {
  auto dir = make_ctx_test_dir("multi_agent");
  ContextManager mgr(dir);

  // Create windows for different agents
  auto &win1 = mgr.get_window(1, 4096);
  auto &win2 = mgr.get_window(2, 8192);
  auto &win3 = mgr.get_window(3, 2048);

  win1.try_add(Message::system("Agent 1 system"));
  win1.try_add(Message::user("Hello agent 1"));

  win2.try_add(Message::system("Agent 2 system"));
  win2.try_add(Message::user("Hello agent 2"));
  win2.try_add(Message::assistant("Hi from agent 2"));

  win3.try_add(Message::user("Hello agent 3"));

  // Each agent has independent message counts
  EXPECT_EQ(mgr.get_window(1).message_count(), 2u);
  EXPECT_EQ(mgr.get_window(2).message_count(), 3u);
  EXPECT_EQ(mgr.get_window(3).message_count(), 1u);

  // Each agent has independent token budgets
  EXPECT_EQ(mgr.get_window(1).max_tokens(), 4096u);
  EXPECT_EQ(mgr.get_window(2).max_tokens(), 8192u);
  EXPECT_EQ(mgr.get_window(3).max_tokens(), 2048u);

  // Clear one agent doesn't affect others
  mgr.clear(2);
  EXPECT_EQ(mgr.get_window(1).message_count(), 2u);
  EXPECT_EQ(mgr.get_window(2).message_count(), 0u); // fresh window
  EXPECT_EQ(mgr.get_window(3).message_count(), 1u);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 8. Context with tool call messages
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, ToolCallMessages) {
  ContextWindow win(4096);

  // System message
  win.try_add(Message::system("You can use tools."));

  // User request
  win.try_add(Message::user("What's the weather?"));

  // Assistant response with tool call
  Message tool_call_msg;
  tool_call_msg.role = Role::Assistant;
  tool_call_msg.content = "";
  tool_call_msg.tool_calls.push_back(ToolCallRequest{
      .id = "call_123",
      .name = "get_weather",
      .args_json = R"({"location":"San Francisco"})",
  });
  EXPECT_TRUE(win.try_add(tool_call_msg));

  // Tool result message
  Message tool_result;
  tool_result.role = Role::Tool;
  tool_result.content = R"({"temp":72,"condition":"sunny"})";
  tool_result.name = "get_weather";
  tool_result.tool_call_id = "call_123";
  EXPECT_TRUE(win.try_add(tool_result));

  // Final assistant message
  win.try_add(Message::assistant("It's 72F and sunny in San Francisco!"));

  EXPECT_EQ(win.message_count(), 5u);

  // Verify the tool call message
  const auto &msgs = win.messages();
  EXPECT_EQ(msgs[2].role, Role::Assistant);
  EXPECT_EQ(msgs[2].tool_calls.size(), 1u);
  EXPECT_EQ(msgs[2].tool_calls[0].name, "get_weather");

  // Verify the tool result message
  EXPECT_EQ(msgs[3].role, Role::Tool);
  EXPECT_EQ(msgs[3].tool_call_id, "call_123");
  EXPECT_EQ(msgs[3].name, "get_weather");
}

// ─────────────────────────────────────────────────────────────
// 9. Context message ordering preservation
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, MessageOrderingPreserved) {
  ContextWindow win(4096);

  // Add messages in a specific order
  std::vector<std::string> expected_order;
  for (int i = 0; i < 20; ++i) {
    std::string content = "msg_" + std::to_string(i);
    expected_order.push_back(content);
    EXPECT_TRUE(win.try_add(Message::user(content)));
  }

  // Verify ordering
  const auto &msgs = win.messages();
  ASSERT_EQ(msgs.size(), 20u);
  for (size_t i = 0; i < msgs.size(); ++i) {
    EXPECT_EQ(msgs[i].content, expected_order[i]);
  }
}

TEST(ContextExtended, AppendPreservesOrdering) {
  auto dir = make_ctx_test_dir("ordering");
  ContextManager mgr(dir);

  // Use append() which internally calls add_evict_if_needed
  for (int i = 0; i < 15; ++i) {
    mgr.append(10, Message::user("append_" + std::to_string(i)));
  }

  const auto &msgs = mgr.get_window(10).messages();
  // Messages should be in insertion order
  for (size_t i = 0; i < msgs.size(); ++i) {
    EXPECT_EQ(msgs[i].content, "append_" + std::to_string(i));
  }

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// 10. Session save/load roundtrip
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, SessionSaveLoadRoundtrip) {
  auto dir = make_ctx_test_dir("session_roundtrip");
  fs::remove_all(dir);

  AgentId aid = 77;

  // Save phase
  {
    ContextManager mgr(dir);
    auto &win = mgr.get_window(aid, 4096);
    win.try_add(Message::system("System prompt"));
    win.try_add(Message::user("Hello"));
    win.try_add(Message::assistant("World"));

    auto save_res = mgr.save_session(aid, R"({"name":"bot77"})",
                                     {"auth_mw", "log_mw"}, R"({"v":1})");
    ASSERT_TRUE(save_res.has_value()) << save_res.error().message;
    EXPECT_TRUE(fs::exists(save_res.value()));
  }

  // Load phase with new ContextManager instance
  {
    ContextManager mgr(dir);

    auto list_res = mgr.list_sessions(aid);
    ASSERT_TRUE(list_res.has_value());
    ASSERT_EQ(list_res->size(), 1u);

    auto load_res = mgr.load_session(aid, (*list_res)[0]);
    ASSERT_TRUE(load_res.has_value()) << load_res.error().message;

    EXPECT_EQ(load_res->agent_id, aid);
    EXPECT_EQ(load_res->config_json, R"({"name":"bot77"})");
    ASSERT_EQ(load_res->middleware_names.size(), 2u);
    EXPECT_EQ(load_res->middleware_names[0], "auth_mw");
    EXPECT_EQ(load_res->middleware_names[1], "log_mw");
    ASSERT_EQ(load_res->context.messages.size(), 3u);
    EXPECT_EQ(load_res->context.messages[0].role, Role::System);
    EXPECT_EQ(load_res->context.messages[0].content, "System prompt");
    EXPECT_EQ(load_res->context.messages[1].content, "Hello");
    EXPECT_EQ(load_res->context.messages[2].content, "World");
  }

  fs::remove_all(dir);
}

TEST(ContextExtended, SessionMultipleSavesAndList) {
  auto dir = make_ctx_test_dir("multi_session");
  fs::remove_all(dir);

  ContextManager mgr(dir);
  AgentId aid = 88;

  auto &win = mgr.get_window(aid, 4096);
  win.try_add(Message::user("First conversation"));
  auto s1 = mgr.save_session(aid, "{}", {});
  ASSERT_TRUE(s1.has_value());

  win.try_add(Message::user("Second conversation"));
  auto s2 = mgr.save_session(aid, "{}", {"mw_extra"});
  ASSERT_TRUE(s2.has_value());

  auto list = mgr.list_sessions(aid);
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list->size(), 2u);

  fs::remove_all(dir);
}

TEST(ContextExtended, LoadNonExistentSessionReturnsError) {
  auto dir = make_ctx_test_dir("noexist_session");
  fs::remove_all(dir);

  ContextManager mgr(dir);
  auto res = mgr.load_session(999, "nonexistent_session_id");
  ASSERT_FALSE(res.has_value());
  EXPECT_EQ(res.error().code, ErrorCode::NotFound);

  fs::remove_all(dir);
}

// ─────────────────────────────────────────────────────────────
// Additional edge cases
// ─────────────────────────────────────────────────────────────

TEST(ContextExtended, IsNearFullThreshold) {
  ContextWindow win(100);
  EXPECT_FALSE(win.is_near_full());

  // Fill up to ~85% of the token budget
  for (int i = 0; i < 25; ++i) {
    win.add_evict_if_needed(Message::user("fill_" + std::to_string(i)));
  }

  // Even if eviction kept usage under max, check the threshold logic
  float util = win.utilization();
  if (util >= 0.85f) {
    EXPECT_TRUE(win.is_near_full());
  } else {
    EXPECT_FALSE(win.is_near_full());
  }
}

TEST(ContextExtended, ClearEvictedMessages) {
  ContextWindow win(80);

  // Force some evictions
  for (int i = 0; i < 30; ++i) {
    win.add_evict_if_needed(Message::user("msg_" + std::to_string(i)));
  }

  EXPECT_FALSE(win.evicted().empty());
  win.clear_evicted();
  EXPECT_TRUE(win.evicted().empty());

  // Main messages should still be intact
  EXPECT_GT(win.message_count(), 0u);
}

TEST(ContextExtended, SnapshotAndRestoreViaManager) {
  auto dir = make_ctx_test_dir("snap_restore");
  fs::remove_all(dir);

  ContextManager mgr(dir);
  AgentId aid = 200;

  auto &win = mgr.get_window(aid, 4096);
  win.try_add(Message::system("Be helpful"));
  win.try_add(Message::user("question"));
  win.try_add(Message::assistant("answer"));

  // Snapshot
  auto snap_path = mgr.snapshot(aid);
  ASSERT_TRUE(snap_path.has_value());
  EXPECT_TRUE(fs::exists(*snap_path));

  // Clear and restore
  mgr.clear(aid);
  EXPECT_EQ(mgr.get_window(aid).message_count(), 0u);

  auto r = mgr.restore(aid);
  ASSERT_TRUE(r);

  auto &restored = mgr.get_window(aid);
  EXPECT_EQ(restored.message_count(), 3u);
  EXPECT_EQ(restored.messages().front().role, Role::System);
  EXPECT_EQ(restored.messages().front().content, "Be helpful");

  fs::remove_all(dir);
}

TEST(ContextExtended, CompressWithSummarizer) {
  auto dir = make_ctx_test_dir("compress");
  fs::remove_all(dir);

  ContextManager mgr(dir);
  AgentId aid = 300;

  mgr.get_window(aid, 200); // small window
  for (int i = 0; i < 30; ++i) {
    mgr.append(aid, Message::user("long_msg_" + std::to_string(i)));
  }

  mgr.compress(aid, [](const std::vector<Message> &) -> std::string {
    return "Compressed summary of conversation.";
  });

  auto &compressed = mgr.get_window(aid);
  EXPECT_LE(compressed.used_tokens(), compressed.max_tokens());

  fs::remove_all(dir);
}

TEST(ContextExtended, EmptyMessageContent) {
  ContextWindow win(4096);
  win.try_add(Message::user(""));
  EXPECT_EQ(win.message_count(), 1u);
  EXPECT_EQ(win.messages().back().content, "");
}

TEST(ContextExtended, InjectSummaryAddsToFront) {
  ContextWindow win(4096);
  win.try_add(Message::user("original message"));

  win.inject_summary("Summary of previous conversation");
  EXPECT_GE(win.message_count(), 2u);
  // Summary should be at the front
  EXPECT_EQ(win.messages().front().role, Role::System);
}
