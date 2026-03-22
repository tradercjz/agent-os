#include <agentos/tools/tool_manager.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::tools;

TEST(ToolTimeoutTest, DefaultTimeoutIs30s) {
    auto os = quickstart_mock();
    auto timeout = os->tools().registry().get_tool_timeout("kv_store");
    EXPECT_EQ(timeout.count(), 30);
}

TEST(ToolTimeoutTest, SetCustomTimeout) {
    auto os = quickstart_mock();
    os->tools().registry().set_tool_timeout("kv_store", std::chrono::seconds(5));
    EXPECT_EQ(os->tools().registry().get_tool_timeout("kv_store").count(), 5);
}

TEST(ToolTimeoutTest, UnknownToolReturnsDefault) {
    auto os = quickstart_mock();
    auto timeout = os->tools().registry().get_tool_timeout("nonexistent");
    EXPECT_EQ(timeout.count(), 30);
}

TEST(ToolTimeoutTest, TimeoutPerTool) {
    auto os = quickstart_mock();
    os->tools().registry().set_tool_timeout("kv_store", std::chrono::seconds(10));
    os->tools().registry().set_tool_timeout("shell_exec", std::chrono::seconds(60));

    EXPECT_EQ(os->tools().registry().get_tool_timeout("kv_store").count(), 10);
    EXPECT_EQ(os->tools().registry().get_tool_timeout("shell_exec").count(), 60);
}
