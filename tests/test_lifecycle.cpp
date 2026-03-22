#include <agentos/core/lifecycle.hpp>
#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(LifecycleManagerTest, EmitAndReceive) {
    LifecycleManager mgr;
    LifecycleEventData received{};
    mgr.on(LifecycleEvent::AgentCreated, [&](const LifecycleEventData& d) {
        received = d;
    });
    mgr.emit({LifecycleEvent::AgentCreated, 42, "test-agent", "created"});
    EXPECT_EQ(received.agent_id, 42u);
    EXPECT_EQ(received.agent_name, "test-agent");
}

TEST(LifecycleManagerTest, FilterByType) {
    LifecycleManager mgr;
    int count = 0;
    mgr.on(LifecycleEvent::AgentCreated, [&](const LifecycleEventData&) { count++; });
    mgr.emit({LifecycleEvent::AgentCreated, 1, "a", ""});
    mgr.emit({LifecycleEvent::AgentDestroyed, 2, "b", ""});
    mgr.emit({LifecycleEvent::AgentCreated, 3, "c", ""});
    EXPECT_EQ(count, 2);
}

TEST(LifecycleManagerTest, AnyListenerReceivesAll) {
    LifecycleManager mgr;
    int count = 0;
    mgr.on_any([&](const LifecycleEventData&) { count++; });
    mgr.emit({LifecycleEvent::AgentCreated, 1, "", ""});
    mgr.emit({LifecycleEvent::AgentDestroyed, 2, "", ""});
    mgr.emit({LifecycleEvent::AgentError, 3, "", ""});
    EXPECT_EQ(count, 3);
}

TEST(LifecycleManagerTest, RecentHistory) {
    LifecycleManager mgr;
    for (int i = 0; i < 5; ++i) {
        mgr.emit({LifecycleEvent::AgentCreated, static_cast<AgentId>(i), "", ""});
    }
    auto recent = mgr.recent(3);
    EXPECT_EQ(recent.size(), 3u);
}

TEST(LifecycleManagerTest, ListenerCount) {
    LifecycleManager mgr;
    EXPECT_EQ(mgr.listener_count(), 0u);
    mgr.on(LifecycleEvent::AgentCreated, [](const LifecycleEventData&) {});
    mgr.on_any([](const LifecycleEventData&) {});
    EXPECT_EQ(mgr.listener_count(), 2u);
}

TEST(LifecycleManagerTest, IntegrationWithAgentOS) {
    auto os = quickstart_mock();
    bool created = false;
    os->lifecycle().on(LifecycleEvent::AgentCreated, [&](const LifecycleEventData& d) {
        created = true;
        EXPECT_EQ(d.agent_name, "lifecycle-test");
    });
    make_agent(*os, "lifecycle-test").prompt("test").create();
    EXPECT_TRUE(created);
}
