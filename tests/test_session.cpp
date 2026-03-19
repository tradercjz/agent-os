#include <agentos/context/context.hpp>
#include <gtest/gtest.h>
#include <filesystem>

namespace fs = std::filesystem;
using namespace agentos;
using namespace agentos::context;

class SessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        snap_dir_ = fs::temp_directory_path() / "test_sessions";
        if (fs::exists(snap_dir_)) fs::remove_all(snap_dir_);
        mgr_ = std::make_unique<ContextManager>(snap_dir_);
    }

    void TearDown() override {
        mgr_.reset();
        if (fs::exists(snap_dir_)) fs::remove_all(snap_dir_);
    }

    fs::path snap_dir_;
    std::unique_ptr<ContextManager> mgr_;
};

// ── SessionState serialization ──

TEST_F(SessionTest, SessionStateRoundTrip) {
    SessionState state;
    state.agent_id = 42;
    state.session_id = "sess-001";
    state.saved_at = TimePoint(Duration(12345));
    state.config_json = R"({"name":"test","context_limit":4096})";
    state.middleware_names = {"logger", "rate-limiter"};
    state.context.agent_id = 42;
    state.context.session_id = "sess-001";
    state.context.captured_at = state.saved_at;
    state.context.messages.push_back(kernel::Message::system("You are a test agent."));
    state.context.messages.push_back(kernel::Message::user("Hello"));
    state.context.metadata_json = R"({"key":"value"})";
    state.metadata_json = R"({"extra":"data"})";

    auto binary = state.serialize_binary();
    ASSERT_FALSE(binary.empty());

    auto restored = SessionState::deserialize_binary(binary);
    ASSERT_TRUE(restored.has_value());
    EXPECT_EQ(restored->agent_id, 42u);
    EXPECT_EQ(restored->session_id, "sess-001");
    EXPECT_EQ(restored->config_json, state.config_json);
    EXPECT_EQ(restored->middleware_names.size(), 2u);
    EXPECT_EQ(restored->middleware_names[0], "logger");
    EXPECT_EQ(restored->middleware_names[1], "rate-limiter");
    EXPECT_EQ(restored->context.messages.size(), 2u);
    EXPECT_EQ(restored->context.messages[0].content, "You are a test agent.");
    EXPECT_EQ(restored->context.messages[1].content, "Hello");
    EXPECT_EQ(restored->metadata_json, state.metadata_json);
}

TEST_F(SessionTest, DeserializeBadMagicReturnsNullopt) {
    std::vector<uint8_t> bad = {'B', 'A', 'D', '!', 0, 0, 0, 1,
                                 0, 0, 0, 0, 0, 0, 0, 0};
    auto result = SessionState::deserialize_binary(bad);
    EXPECT_FALSE(result.has_value());
}

TEST_F(SessionTest, DeserializeTooSmallReturnsNullopt) {
    std::vector<uint8_t> tiny = {1, 2, 3};
    auto result = SessionState::deserialize_binary(tiny);
    EXPECT_FALSE(result.has_value());
}

// ── ContextManager session methods ──

TEST_F(SessionTest, SaveAndLoadSession) {
    AgentId aid = 1;
    auto& win = mgr_->get_window(aid, 4096);
    win.try_add(kernel::Message::system("System prompt"));
    win.try_add(kernel::Message::user("User input"));

    auto save_res = mgr_->save_session(aid, R"({"name":"bot"})", {"mw1", "mw2"});
    ASSERT_TRUE(save_res.has_value()) << save_res.error().message;
    EXPECT_TRUE(fs::exists(save_res.value()));

    // List sessions
    auto list_res = mgr_->list_sessions(aid);
    ASSERT_TRUE(list_res.has_value());
    ASSERT_EQ(list_res->size(), 1u);

    // Load the session
    auto load_res = mgr_->load_session(aid, (*list_res)[0]);
    ASSERT_TRUE(load_res.has_value()) << load_res.error().message;
    EXPECT_EQ(load_res->agent_id, aid);
    EXPECT_EQ(load_res->config_json, R"({"name":"bot"})");
    EXPECT_EQ(load_res->middleware_names.size(), 2u);
    EXPECT_EQ(load_res->context.messages.size(), 2u);
    EXPECT_EQ(load_res->context.messages[0].content, "System prompt");
}

TEST_F(SessionTest, LoadNonExistentReturnsNotFound) {
    auto res = mgr_->load_session(999, "nonexistent");
    ASSERT_FALSE(res.has_value());
    EXPECT_EQ(res.error().code, ErrorCode::NotFound);
}

TEST_F(SessionTest, ListSessionsEmptyAgent) {
    auto res = mgr_->list_sessions(999);
    ASSERT_TRUE(res.has_value());
    EXPECT_TRUE(res->empty());
}

TEST_F(SessionTest, MultipleSessions) {
    AgentId aid = 2;
    auto& win = mgr_->get_window(aid, 4096);
    win.try_add(kernel::Message::user("First"));

    auto s1 = mgr_->save_session(aid, "{}", {});
    ASSERT_TRUE(s1.has_value());

    win.try_add(kernel::Message::user("Second"));
    auto s2 = mgr_->save_session(aid, "{}", {"extra-mw"});
    ASSERT_TRUE(s2.has_value());

    auto list = mgr_->list_sessions(aid);
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->size(), 2u);
}
