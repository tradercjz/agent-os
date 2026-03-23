#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <string>

using namespace agentos;
namespace fs = std::filesystem;

namespace {

fs::path make_conv_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("agentos_conv_test_" + name + "_" + std::to_string(nonce));
}

} // namespace

// ── Fixture ─────────────────────────────────────────────────

class ConvenienceAPITest : public ::testing::Test {
protected:
  fs::path test_dir_ = make_conv_test_dir("conv");

  std::unique_ptr<AgentOS> make_os() {
    return AgentOSBuilder()
        .mock()
        .security(false)
        .data_dir(test_dir_.string())
        .build();
  }

  void TearDown() override { fs::remove_all(test_dir_); }
};

// ── kill_all_agents ─────────────────────────────────────────

TEST_F(ConvenienceAPITest, KillAllAgents_Empty) {
  auto os = make_os();
  EXPECT_EQ(os->kill_all_agents(), 0u);
  EXPECT_EQ(os->agent_count(), 0u);
}

TEST_F(ConvenienceAPITest, KillAllAgents_One) {
  auto os = make_os();
  AgentConfig cfg;
  cfg.name = "solo";
  (void)os->create_agent(cfg);
  EXPECT_EQ(os->agent_count(), 1u);

  EXPECT_EQ(os->kill_all_agents(), 1u);
  EXPECT_EQ(os->agent_count(), 0u);
}

TEST_F(ConvenienceAPITest, KillAllAgents_Five) {
  auto os = make_os();
  for (int i = 0; i < 5; ++i) {
    AgentConfig cfg;
    cfg.name = "agent-" + std::to_string(i);
    (void)os->create_agent(cfg);
  }
  EXPECT_EQ(os->agent_count(), 5u);

  EXPECT_EQ(os->kill_all_agents(), 5u);
  EXPECT_EQ(os->agent_count(), 0u);
}

// ── find_agents_by_name ─────────────────────────────────────

TEST_F(ConvenienceAPITest, FindAgentsByName_Match) {
  auto os = make_os();
  AgentConfig cfg1;
  cfg1.name = "alpha";
  (void)os->create_agent(cfg1);

  AgentConfig cfg2;
  cfg2.name = "beta";
  (void)os->create_agent(cfg2);

  AgentConfig cfg3;
  cfg3.name = "alpha";
  (void)os->create_agent(cfg3);

  auto found = os->find_agents_by_name("alpha");
  EXPECT_EQ(found.size(), 2u);
  for (const auto &a : found) {
    EXPECT_EQ(a->config().name, "alpha");
  }
}

TEST_F(ConvenienceAPITest, FindAgentsByName_NoMatch) {
  auto os = make_os();
  AgentConfig cfg;
  cfg.name = "alpha";
  (void)os->create_agent(cfg);

  auto found = os->find_agents_by_name("gamma");
  EXPECT_TRUE(found.empty());
}

// ── max_agents enforcement ──────────────────────────────────

TEST_F(ConvenienceAPITest, MaxAgents_EnforceLimit) {
  auto os = AgentOSBuilder()
      .mock()
      .security(false)
      .max_agents(2)
      .data_dir(test_dir_.string())
      .build();

  AgentConfig cfg1;
  cfg1.name = "a1";
  EXPECT_NO_THROW(os->create_agent(cfg1));

  AgentConfig cfg2;
  cfg2.name = "a2";
  EXPECT_NO_THROW(os->create_agent(cfg2));

  // Third agent should fail
  AgentConfig cfg3;
  cfg3.name = "a3";
  EXPECT_THROW(os->create_agent(cfg3), std::runtime_error);

  EXPECT_EQ(os->agent_count(), 2u);
}

TEST_F(ConvenienceAPITest, MaxAgents_ZeroMeansUnlimited) {
  auto os = AgentOSBuilder()
      .mock()
      .security(false)
      .max_agents(0)
      .data_dir(test_dir_.string())
      .build();

  for (int i = 0; i < 10; ++i) {
    AgentConfig cfg;
    cfg.name = "agent-" + std::to_string(i);
    EXPECT_NO_THROW(os->create_agent(cfg));
  }
  EXPECT_EQ(os->agent_count(), 10u);
}

// ── Builder with tracing ────────────────────────────────────

TEST_F(ConvenienceAPITest, BuilderTracingEnabled) {
  auto os = AgentOSBuilder()
      .mock()
      .security(false)
      .tracing(true)
      .data_dir(test_dir_.string())
      .build();
  EXPECT_TRUE(os->tracer().enabled());
}

TEST_F(ConvenienceAPITest, BuilderTracingDisabled) {
  auto os = AgentOSBuilder()
      .mock()
      .security(false)
      .tracing(false)
      .data_dir(test_dir_.string())
      .build();
  EXPECT_FALSE(os->tracer().enabled());
}
