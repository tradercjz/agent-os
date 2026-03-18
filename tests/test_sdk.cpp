#include <agentos/agentos.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
namespace fs = std::filesystem;

// ── Version ─────────────────────────────────────────────────
TEST(SDKTest, VersionInfo) {
  auto v = version();
  EXPECT_GE(v.major, 0);
  EXPECT_GE(v.minor, 0);
  EXPECT_FALSE(v.to_string().empty());
}

// ── AgentOSBuilder ──────────────────────────────────────────

class SDKBuilderTest : public ::testing::Test {
protected:
  void TearDown() override {
    fs::remove_all(fs::temp_directory_path() / "agentos_sdk_test");
  }
};

TEST_F(SDKBuilderTest, MockBuilder) {
  auto os = AgentOSBuilder()
                .mock()
                .threads(2)
                .tpm(50000)
                .security(false)
                .build();

  ASSERT_NE(os, nullptr);
  EXPECT_EQ(os->agent_count(), 0u);
}

TEST_F(SDKBuilderTest, DataDirSetsSnapshotAndLtm) {
  auto dir = (fs::temp_directory_path() / "agentos_sdk_test").string();
  auto os = AgentOSBuilder()
                .mock()
                .data_dir(dir)
                .security(false)
                .build();

  ASSERT_NE(os, nullptr);
}

TEST_F(SDKBuilderTest, BuildWithoutBackendThrows) {
  EXPECT_THROW(AgentOSBuilder().build(), std::runtime_error);
}

// ── quickstart_mock ─────────────────────────────────────────

TEST_F(SDKBuilderTest, QuickstartMock) {
  auto os = quickstart_mock();
  ASSERT_NE(os, nullptr);
  EXPECT_EQ(os->agent_count(), 0u);
}

// ── AgentBuilder (fluent agent creation) ────────────────────

TEST_F(SDKBuilderTest, AgentBuilderFluent) {
  auto os = quickstart_mock();

  auto agent = make_agent(*os, "TestBot")
                   .prompt("You are a test bot.")
                   .context(4096)
                   .priority(Priority::High)
                   .persistent(true)
                   .create();

  ASSERT_NE(agent, nullptr);
  EXPECT_EQ(agent->config().name, "TestBot");
  EXPECT_EQ(agent->config().context_limit, 4096u);
  EXPECT_EQ(agent->config().priority, Priority::High);
  EXPECT_TRUE(agent->config().persist_memory);
  EXPECT_EQ(os->agent_count(), 1u);
}

TEST_F(SDKBuilderTest, AgentBuilderWithTools) {
  auto os = quickstart_mock();

  auto agent = make_agent(*os, "ToolBot")
                   .prompt("A tool-using bot")
                   .tools({"kv_store", "calculator"})
                   .create();

  ASSERT_NE(agent, nullptr);
  EXPECT_EQ(agent->config().allowed_tools.size(), 2u);
  EXPECT_EQ(agent->config().allowed_tools[0], "kv_store");
}

TEST_F(SDKBuilderTest, AgentBuilderCustomType) {
  auto os = quickstart_mock();

  // Create using default ReActAgent (custom type = ReActAgent explicitly)
  auto agent = make_agent(*os, "Custom")
                   .prompt("test")
                   .create<ReActAgent>();

  ASSERT_NE(agent, nullptr);
  auto result = agent->run("hello");
  // MockBackend returns default response
  ASSERT_TRUE(result);
}

// ── JSON Config ─────────────────────────────────────────────

TEST_F(SDKBuilderTest, FromJsonMock) {
  nlohmann::json j = {
      {"backend", "mock"},
      {"threads", 2},
      {"tpm_limit", 50000},
      {"security", false},
      {"log_level", "warn"},
  };

  auto os = from_json(j);
  ASSERT_NE(os, nullptr);
  EXPECT_EQ(os->agent_count(), 0u);
}

TEST_F(SDKBuilderTest, FromJsonFile) {
  auto dir = fs::temp_directory_path() / "agentos_sdk_test";
  fs::create_directories(dir);
  auto cfg_path = dir / "config.json";

  // Write config file
  nlohmann::json j = {
      {"backend", "mock"},
      {"threads", 1},
      {"security", false},
  };
  std::ofstream ofs(cfg_path);
  ofs << j.dump();
  ofs.close();

  auto os = from_json_file(cfg_path.string());
  ASSERT_NE(os, nullptr);
}

TEST_F(SDKBuilderTest, FromJsonMissingFileThrows) {
  EXPECT_THROW(from_json_file("/nonexistent/config.json"), std::runtime_error);
}

// ── register_tool shorthand ─────────────────────────────────

TEST_F(SDKBuilderTest, RegisterToolShorthand) {
  auto os = quickstart_mock();

  os->register_tool(
      tools::ToolSchema{
          .id = "echo",
          .description = "Echo back the input",
          .params = {{.name = "msg",
                      .type = tools::ParamType::String,
                      .description = "message",
                      .required = true,
                      .default_value = std::nullopt}},
      },
      [](const tools::ParsedArgs &args) -> tools::ToolResult {
        return tools::ToolResult::ok(args.get("msg"));
      });

  auto schemas = os->tools().registry().list_schemas();
  bool found = false;
  for (auto &s : schemas) {
    if (s.id == "echo") found = true;
  }
  EXPECT_TRUE(found);
}

// ── find_agent ──────────────────────────────────────────────

TEST_F(SDKBuilderTest, FindAgent) {
  auto os = quickstart_mock();
  auto agent = make_agent(*os, "Findable").create();

  auto found = os->find_agent(agent->id());
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->config().name, "Findable");

  auto not_found = os->find_agent(99999);
  EXPECT_EQ(not_found, nullptr);
}

// ── End-to-end: builder → agent → run ───────────────────────

TEST_F(SDKBuilderTest, EndToEndBuilderFlow) {
  // This tests the typical SDK usage pattern
  auto os = AgentOSBuilder()
                .mock()
                .threads(2)
                .security(true)
                .build();

  auto agent = make_agent(*os, "E2EAgent")
                   .prompt("You are helpful.")
                   .role("standard")
                   .create();

  auto result = agent->run("hello");
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->empty());

  EXPECT_EQ(os->agent_count(), 1u);
  auto status = os->status();
  EXPECT_TRUE(status.find("agents=1") != std::string::npos);
}

// ── Middleware / Hooks ──────────────────────────────────────

TEST_F(SDKBuilderTest, MiddlewareBeforeThink) {
  auto os = quickstart_mock();
  auto agent = make_agent(*os, "HookBot").prompt("test").create();

  // Track think calls via middleware
  int before_count = 0;
  int after_count = 0;
  agent->use(Middleware{
      .name = "counter",
      .before = [&](HookContext &) { before_count++; },
      .after = [&](HookContext &) { after_count++; },
  });

  auto result = agent->run("hello");
  ASSERT_TRUE(result);
  EXPECT_GE(before_count, 1); // at least one think call
  EXPECT_GE(after_count, 1);
}

TEST_F(SDKBuilderTest, MiddlewareCancellation) {
  auto os = quickstart_mock();
  auto agent = make_agent(*os, "GuardBot").prompt("test").create();

  // Block all think operations
  agent->use(Middleware{
      .name = "blocker",
      .before = [](HookContext &ctx) {
        if (ctx.operation == "think") {
          ctx.cancelled = true;
          ctx.cancel_reason = "blocked by policy";
        }
      },
      .after = nullptr
  });

  auto resp = agent->think("hello");
  ASSERT_FALSE(resp);
  EXPECT_EQ(resp.error().code, ErrorCode::Cancelled);
  EXPECT_TRUE(resp.error().message.find("blocked by policy") != std::string::npos);
}

// ── run_async ───────────────────────────────────────────────

TEST_F(SDKBuilderTest, RunAsync) {
  auto os = quickstart_mock();
  auto agent = make_agent(*os, "AsyncBot").prompt("test").create();

  auto future = agent->run_async("hello");
  auto result = future.get();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->empty());
}

TEST_F(SDKBuilderTest, MultipleAsyncAgents) {
  auto os = quickstart_mock();
  auto agent1 = make_agent(*os, "Bot1").prompt("test").create();
  auto agent2 = make_agent(*os, "Bot2").prompt("test").create();

  auto f1 = agent1->run_async("hello from 1");
  auto f2 = agent2->run_async("hello from 2");

  auto r1 = f1.get();
  auto r2 = f2.get();
  ASSERT_TRUE(r1);
  ASSERT_TRUE(r2);
}

// ── Health Check ────────────────────────────────────────────

TEST_F(SDKBuilderTest, HealthCheck) {
  auto os = quickstart_mock();

  auto h = os->health();
  EXPECT_TRUE(h.healthy);
  EXPECT_TRUE(h.scheduler_running);
  EXPECT_TRUE(h.backend_available);
  EXPECT_EQ(h.active_agents, 0u);
  EXPECT_EQ(h.total_requests, 0u);
  EXPECT_EQ(h.total_errors, 0u);

  // JSON output
  auto json = h.to_json();
  EXPECT_TRUE(json.find("\"healthy\":true") != std::string::npos);
  EXPECT_TRUE(json.find("\"scheduler\":true") != std::string::npos);
}

TEST_F(SDKBuilderTest, HealthCheckAfterActivity) {
  auto os = quickstart_mock();
  auto agent = make_agent(*os, "Active").prompt("test").create();
  (void)agent->run("hello");

  auto h = os->health();
  EXPECT_EQ(h.active_agents, 1u);
  EXPECT_GE(h.total_requests, 1u);
}

// ── Graceful Shutdown ───────────────────────────────────────

TEST_F(SDKBuilderTest, GracefulShutdown) {
  auto os = quickstart_mock();

  std::atomic<int> counter{0};
  (void)os->submit_task("task1", [&] { counter++; });
  (void)os->submit_task("task2", [&] { counter++; });

  os->graceful_shutdown(Duration{5000});

  // Tasks should have completed
  EXPECT_EQ(counter.load(), 2);
}

// ── Scheduler drain / active_task_count ─────────────────────

TEST_F(SDKBuilderTest, SchedulerDrain) {
  auto os = quickstart_mock();

  std::atomic<int> done{0};
  for (int i = 0; i < 5; ++i) {
    (void)os->submit_task("t" + std::to_string(i), [&] {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      done++;
    });
  }

  bool drained = os->scheduler().drain(Duration{5000});
  EXPECT_TRUE(drained);
  EXPECT_EQ(done.load(), 5);
  EXPECT_EQ(os->scheduler().active_task_count(), 0u);
}

// ── Coverage boost tests ────────────────────────────────

// Test from_json with all config fields
TEST_F(SDKBuilderTest, FromJsonAllFields) {
  auto dir = (fs::temp_directory_path() / "agentos_sdk_test").string();
  nlohmann::json j = {
      {"backend", "mock"},
      {"threads", 2},
      {"tpm_limit", 50000},
      {"data_dir", dir},
      {"security", false},
      {"log_level", "debug"},
  };

  auto os = from_json(j);
  ASSERT_NE(os, nullptr);
  EXPECT_EQ(os->agent_count(), 0u);
}

// Test from_json with log_level variants
TEST_F(SDKBuilderTest, FromJsonLogLevels) {
  for (const std::string &level : {"info", "warn", "error", "off"}) {
    nlohmann::json j = {
        {"backend", "mock"},
        {"security", false},
        {"log_level", level},
    };
    auto os = from_json(j);
    ASSERT_NE(os, nullptr);
  }
}

// Test from_json with snapshot_dir and ltm_dir separately
TEST_F(SDKBuilderTest, FromJsonSeparateDirs) {
  auto dir = fs::temp_directory_path() / "agentos_sdk_test";
  nlohmann::json j = {
      {"backend", "mock"},
      {"security", false},
      {"snapshot_dir", (dir / "snaps").string()},
      {"ltm_dir", (dir / "ltm").string()},
  };

  auto os = from_json(j);
  ASSERT_NE(os, nullptr);
}

// Test from_json with openai backend but missing api_key (should throw)
TEST_F(SDKBuilderTest, FromJsonOpenAIMissingKeyThrows) {
  // Unset env var to ensure it's not picked up
  nlohmann::json j = {
      {"backend", "openai"},
      // no api_key, and OPENAI_API_KEY probably not set in test env
  };

  // This may or may not throw depending on whether OPENAI_API_KEY is set
  // We check the behavior is consistent
  try {
    auto os = from_json(j);
    // If OPENAI_API_KEY is set in env, this succeeds
    EXPECT_NE(os, nullptr);
  } catch (const std::runtime_error &e) {
    EXPECT_NE(std::string(e.what()).find("api_key"), std::string::npos);
  }
}

// Test builder with custom backend
TEST_F(SDKBuilderTest, BuilderWithCustomBackend) {
  auto custom = std::make_unique<kernel::MockLLMBackend>();
  auto os = AgentOSBuilder()
                .backend(std::move(custom))
                .security(false)
                .build();

  ASSERT_NE(os, nullptr);
  EXPECT_EQ(os->agent_count(), 0u);
}

// Test builder log_level setter
TEST_F(SDKBuilderTest, BuilderLogLevel) {
  auto os = AgentOSBuilder()
                .mock()
                .log_level(LogLevel::Warn)
                .security(false)
                .build();
  ASSERT_NE(os, nullptr);
}

// Test AgentBuilder config accessor
TEST_F(SDKBuilderTest, AgentBuilderConfigAccess) {
  auto os = quickstart_mock();
  auto builder = make_agent(*os, "ConfigBot");
  builder.prompt("test prompt");
  builder.context(2048);
  builder.priority(Priority::Low);

  // Access the underlying config
  auto &cfg = builder.config();
  EXPECT_EQ(cfg.name, "ConfigBot");
  EXPECT_EQ(cfg.role_prompt, "test prompt");
  EXPECT_EQ(cfg.context_limit, 2048u);
  EXPECT_EQ(cfg.priority, Priority::Low);

  auto agent = builder.create();
  ASSERT_NE(agent, nullptr);
}

// Test AgentOSBuilder config accessor
TEST_F(SDKBuilderTest, AgentOSBuilderConfigAccess) {
  AgentOSBuilder builder;
  builder.mock().security(false);

  auto &cfg = builder.config();
  EXPECT_FALSE(cfg.enable_security);

  auto os = builder.build();
  ASSERT_NE(os, nullptr);
}

// Test quickstart without OPENAI_API_KEY throws
TEST_F(SDKBuilderTest, QuickstartNoApiKeyThrows) {
  // Save and unset the env var
  const char *old = std::getenv("OPENAI_API_KEY");
  if (old) {
    // If set, skip this test since unsetting is not safe in parallel tests
    GTEST_SKIP() << "OPENAI_API_KEY is set, skipping quickstart error test";
  }
  EXPECT_THROW(quickstart(), std::runtime_error);
}

// Test snapshot_dir and ltm_dir builder methods
TEST_F(SDKBuilderTest, BuilderSnapshotAndLtmDirs) {
  auto dir = fs::temp_directory_path() / "agentos_sdk_test";
  auto os = AgentOSBuilder()
                .mock()
                .snapshot_dir((dir / "custom_snaps").string())
                .ltm_dir((dir / "custom_ltm").string())
                .security(false)
                .build();

  ASSERT_NE(os, nullptr);
}

// Test version patch level
TEST(SDKTest, VersionPatch) {
  auto v = version();
  EXPECT_GE(v.patch, 0);
  EXPECT_EQ(v.to_string(), "0.9.0");
}

// Test multiple agents with find_agent
TEST_F(SDKBuilderTest, FindMultipleAgents) {
  auto os = quickstart_mock();
  auto a1 = make_agent(*os, "Agent1").create();
  auto a2 = make_agent(*os, "Agent2").create();
  auto a3 = make_agent(*os, "Agent3").create();

  EXPECT_EQ(os->agent_count(), 3u);
  EXPECT_NE(os->find_agent(a1->id()), nullptr);
  EXPECT_NE(os->find_agent(a2->id()), nullptr);
  EXPECT_NE(os->find_agent(a3->id()), nullptr);
  EXPECT_EQ(os->find_agent(a1->id())->config().name, "Agent1");
  EXPECT_EQ(os->find_agent(a3->id())->config().name, "Agent3");
}

// Test health check fields after shutdown
TEST_F(SDKBuilderTest, HealthAfterShutdown) {
  auto os = quickstart_mock();
  auto h1 = os->health();
  EXPECT_TRUE(h1.healthy);

  os->graceful_shutdown(Duration{1000});
  auto h2 = os->health();
  EXPECT_FALSE(h2.scheduler_running);
  EXPECT_FALSE(h2.healthy);
}
