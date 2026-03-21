// ============================================================
// Tests :: Per-Agent Rate Limiter
// ============================================================
#include <agentos/core/agent_rate_limiter.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(AgentRateLimiter, SetAndGetQuota) {
    AgentRateLimiter rl;
    rl.set_quota(1, AgentQuota{.tpm = 1000});
    EXPECT_TRUE(rl.has_quota(1));
    EXPECT_FALSE(rl.has_quota(2));
}

TEST(AgentRateLimiter, GetLimiterReturnsNullForNoQuota) {
    AgentRateLimiter rl;
    EXPECT_EQ(rl.get_limiter(42), nullptr);
}

TEST(AgentRateLimiter, GetLimiterReturnsLimiterForSetQuota) {
    AgentRateLimiter rl;
    rl.set_quota(1, AgentQuota{.tpm = 5000});
    auto* limiter = rl.get_limiter(1);
    EXPECT_NE(limiter, nullptr);
}

TEST(AgentRateLimiter, RemoveQuota) {
    AgentRateLimiter rl;
    rl.set_quota(1, AgentQuota{.tpm = 1000});
    EXPECT_TRUE(rl.has_quota(1));
    rl.remove_quota(1);
    EXPECT_FALSE(rl.has_quota(1));
    EXPECT_EQ(rl.get_limiter(1), nullptr);
}

TEST(AgentRateLimiter, ZeroTPMRemovesLimiter) {
    AgentRateLimiter rl;
    rl.set_quota(1, AgentQuota{.tpm = 1000});
    EXPECT_NE(rl.get_limiter(1), nullptr);

    // Setting tpm=0 removes the limiter but quota entry remains
    rl.set_quota(1, AgentQuota{.tpm = 0});
    EXPECT_EQ(rl.get_limiter(1), nullptr);
    EXPECT_TRUE(rl.has_quota(1));  // quota entry exists with tpm=0
}

TEST(AgentRateLimiter, MultipleAgents) {
    AgentRateLimiter rl;
    rl.set_quota(1, AgentQuota{.tpm = 1000});
    rl.set_quota(2, AgentQuota{.tpm = 2000});
    rl.set_quota(3, AgentQuota{.tpm = 3000});

    auto* l1 = rl.get_limiter(1);
    auto* l2 = rl.get_limiter(2);
    auto* l3 = rl.get_limiter(3);

    EXPECT_NE(l1, nullptr);
    EXPECT_NE(l2, nullptr);
    EXPECT_NE(l3, nullptr);
    EXPECT_NE(l1, l2);
    EXPECT_NE(l2, l3);
}

TEST(AgentRateLimiter, AgentCount) {
    AgentRateLimiter rl;
    EXPECT_EQ(rl.agent_count(), 0u);

    rl.set_quota(1, AgentQuota{.tpm = 1000});
    EXPECT_EQ(rl.agent_count(), 1u);

    rl.set_quota(2, AgentQuota{.tpm = 2000});
    EXPECT_EQ(rl.agent_count(), 2u);

    rl.remove_quota(1);
    EXPECT_EQ(rl.agent_count(), 1u);
}

TEST(AgentRateLimiter, QuotasReturnsAll) {
    AgentRateLimiter rl;
    rl.set_quota(10, AgentQuota{.tpm = 500});
    rl.set_quota(20, AgentQuota{.tpm = 1500});

    auto all = rl.quotas();
    EXPECT_EQ(all.size(), 2u);
    EXPECT_EQ(all[10].tpm, 500u);
    EXPECT_EQ(all[20].tpm, 1500u);
}
