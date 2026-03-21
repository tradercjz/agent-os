#include <agentos/context/conversation_store.hpp>
#include <filesystem>
#include <gtest/gtest.h>

using namespace agentos;

TEST(ConversationStoreTest, SaveAndLoad) {
    auto dir = (std::filesystem::temp_directory_path() / "test_conv_store").string();
    std::filesystem::create_directories(dir);

    context::ConversationStore store(dir);
    context::ConversationRecord record;
    record.conversation_id = "conv-1";
    record.agent_id = 42;
    record.agent_name = "test-agent";
    record.messages.push_back(kernel::Message::system("You are helpful"));
    record.messages.push_back(kernel::Message::user("Hello"));
    record.messages.push_back(kernel::Message::assistant("Hi there!"));
    record.created_at = "2026-03-21T00:00:00Z";
    record.updated_at = "2026-03-21T00:01:00Z";

    auto sr = store.save(record);
    ASSERT_TRUE(sr.has_value());

    auto lr = store.load("conv-1");
    ASSERT_TRUE(lr.has_value());
    EXPECT_EQ(lr->agent_name, "test-agent");
    EXPECT_EQ(lr->messages.size(), 3u);
    EXPECT_EQ(lr->messages[0].role, kernel::Role::System);
    EXPECT_EQ(lr->messages[2].content, "Hi there!");

    std::filesystem::remove_all(dir);
}

TEST(ConversationStoreTest, ListConversations) {
    auto dir = (std::filesystem::temp_directory_path() / "test_conv_list").string();
    std::filesystem::create_directories(dir);

    context::ConversationStore store(dir);
    for (int i = 0; i < 3; ++i) {
        context::ConversationRecord r;
        r.conversation_id = "conv-" + std::to_string(i);
        r.agent_id = 1;
        (void)store.save(r);
    }

    auto ids = store.list();
    EXPECT_EQ(ids.size(), 3u);
    EXPECT_EQ(store.count(), 3u);

    std::filesystem::remove_all(dir);
}

TEST(ConversationStoreTest, RemoveConversation) {
    auto dir = (std::filesystem::temp_directory_path() / "test_conv_rm").string();
    std::filesystem::create_directories(dir);

    context::ConversationStore store(dir);
    context::ConversationRecord r;
    r.conversation_id = "to-delete";
    (void)store.save(r);
    EXPECT_EQ(store.count(), 1u);

    (void)store.remove("to-delete");
    EXPECT_EQ(store.count(), 0u);

    std::filesystem::remove_all(dir);
}

TEST(ConversationStoreTest, LoadNonexistentReturnsError) {
    auto tmp = (std::filesystem::temp_directory_path() / "empty_conv_dir_12345").string();
    std::filesystem::create_directories(tmp);
    context::ConversationStore store(tmp);
    auto r = store.load("nonexistent");
    EXPECT_FALSE(r.has_value());
    std::filesystem::remove_all(tmp);
}

TEST(ConversationStoreTest, ToolMessageRoundTrip) {
    auto dir = (std::filesystem::temp_directory_path() / "test_conv_tool").string();
    std::filesystem::create_directories(dir);

    context::ConversationStore store(dir);
    context::ConversationRecord record;
    record.conversation_id = "conv-tool";
    record.agent_id = 1;

    kernel::Message tool_msg;
    tool_msg.role = kernel::Role::Tool;
    tool_msg.content = R"({"result": 42})";
    tool_msg.tool_call_id = "call_123";
    tool_msg.name = "calculator";
    record.messages.push_back(std::move(tool_msg));

    (void)store.save(record);

    auto lr = store.load("conv-tool");
    ASSERT_TRUE(lr.has_value());
    ASSERT_EQ(lr->messages.size(), 1u);
    EXPECT_EQ(lr->messages[0].role, kernel::Role::Tool);
    EXPECT_EQ(lr->messages[0].tool_call_id, "call_123");
    EXPECT_EQ(lr->messages[0].name, "calculator");

    std::filesystem::remove_all(dir);
}

TEST(ConversationStoreTest, OverwriteExistingConversation) {
    auto dir = (std::filesystem::temp_directory_path() / "test_conv_overwrite").string();
    std::filesystem::create_directories(dir);

    context::ConversationStore store(dir);
    context::ConversationRecord r;
    r.conversation_id = "conv-ow";
    r.agent_name = "original";
    (void)store.save(r);

    r.agent_name = "updated";
    r.messages.push_back(kernel::Message::user("new msg"));
    (void)store.save(r);

    auto lr = store.load("conv-ow");
    ASSERT_TRUE(lr.has_value());
    EXPECT_EQ(lr->agent_name, "updated");
    EXPECT_EQ(lr->messages.size(), 1u);
    EXPECT_EQ(store.count(), 1u);

    std::filesystem::remove_all(dir);
}
