#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

TEST(AgentOSCoreTest, VersionCheck) {
    auto v = agentos::version();
    EXPECT_EQ(v.major, AGENTOS_VERSION_MAJOR);
    EXPECT_EQ(v.minor, AGENTOS_VERSION_MINOR);
    EXPECT_EQ(v.patch, AGENTOS_VERSION_PATCH);
    EXPECT_EQ(v.to_string(), std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch));
}
