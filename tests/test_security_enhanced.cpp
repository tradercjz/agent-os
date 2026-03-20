// ============================================================
// AgentOS Enhanced Security Tests
// SSRF protection, shell sanitization, injection hot-reload
// ============================================================
#include <agentos/security/security.hpp>
#include <agentos/memory/graph_memory.hpp>
#include <agentos/tools/tool_manager.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <string>

using namespace agentos;
using namespace agentos::security;
using namespace agentos::tools;

namespace {

std::filesystem::path make_security_wal_test_dir(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return std::filesystem::temp_directory_path() /
         ("agentos_test_wal_" + name + "_" + std::to_string(nonce));
}

}

// ── InjectionDetector Hot-Reload Tests ──────────────────────

TEST(InjectionHotReload, AddPatternAtRuntime) {
  InjectionDetector detector;
  size_t initial = detector.pattern_count();

  detector.add_pattern("new evil pattern");
  EXPECT_EQ(detector.pattern_count(), initial + 1);

  auto r = detector.scan("this contains new evil pattern");
  EXPECT_TRUE(r.is_injection);
}

TEST(InjectionHotReload, RemovePatternAtRuntime) {
  InjectionDetector detector;
  detector.add_pattern("removable pattern");
  EXPECT_TRUE(detector.remove_pattern("removable pattern"));
  EXPECT_FALSE(detector.remove_pattern("nonexistent"));

  auto r = detector.scan("removable pattern here");
  EXPECT_FALSE(r.is_injection); // Should not match after removal
}

TEST(InjectionHotReload, ReplaceAllPatterns) {
  InjectionDetector detector;

  detector.set_patterns({"custom_only_pattern"});
  EXPECT_EQ(detector.pattern_count(), 1u);

  // Original patterns should be gone
  auto r1 = detector.scan("ignore previous instructions");
  EXPECT_FALSE(r1.is_injection);

  // Custom pattern should work
  auto r2 = detector.scan("this has custom_only_pattern");
  EXPECT_TRUE(r2.is_injection);
}

TEST(InjectionHotReload, ConcurrentAccess) {
  InjectionDetector detector;
  std::atomic<int> detections{0};

  // Writer thread: continuously add patterns
  std::thread writer([&] {
    for (int i = 0; i < 100; ++i) {
      detector.add_pattern("concurrent_pattern_" + std::to_string(i));
    }
  });

  // Reader threads: continuously scan
  std::vector<std::thread> readers;
  for (int t = 0; t < 4; ++t) {
    readers.emplace_back([&] {
      for (int i = 0; i < 100; ++i) {
        auto r = detector.scan("concurrent_pattern_" + std::to_string(i));
        if (r.is_injection) detections++;
      }
    });
  }

  writer.join();
  for (auto &r : readers) r.join();

  // Some detections should have occurred (exact count depends on timing)
  EXPECT_GT(detections.load(), 0);
}

// ── ShellTool Sanitization Tests ────────────────────────────

TEST(ShellToolSecurity, BlocksUnsafeCharacters) {
  ShellTool shell;

  // All these should be blocked
  std::vector<std::string> unsafe_cmds = {
    "echo hello; rm -rf /",       // semicolon chain
    "echo $(whoami)",             // command substitution
    "echo `id`",                  // backtick substitution
    "echo hello | cat",           // pipe
    "echo hello & bg",            // background
    "echo hello > /etc/passwd",   // redirect
    "echo hello < /etc/shadow",   // input redirect
    "echo \"hello\"",             // double quotes
    "echo 'hello'",               // single quotes
    "echo hello!",                // history expansion
    "echo {a,b}",                 // brace expansion
    "echo hello#comment",         // comment
    "echo ~root",                 // tilde expansion
  };

  for (const auto &cmd : unsafe_cmds) {
    ParsedArgs args;
    args.values["cmd"] = cmd;
    auto result = shell.execute(args);
    EXPECT_FALSE(result.success) << "Should block: " << cmd;
  }
}

TEST(ShellToolSecurity, AllowsSafeCommands) {
  ShellTool shell;

  ParsedArgs args;
  args.values["cmd"] = "echo hello world";
  auto result = shell.execute(args);
  EXPECT_TRUE(result.success);
  EXPECT_NE(result.output.find("hello world"), std::string::npos);
}

TEST(ShellToolSecurity, BlocksNonWhitelisted) {
  ShellTool shell;

  ParsedArgs args;
  args.values["cmd"] = "rm -rf /tmp/test";
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("not in allowlist"), std::string::npos);
}

TEST(ShellToolSecurity, BlocksTooManyArguments) {
  ShellTool shell;

  // Build a command with >20 args
  std::string cmd = "echo";
  for (int i = 0; i < 25; ++i) cmd += " arg" + std::to_string(i);

  ParsedArgs args;
  args.values["cmd"] = cmd;
  auto result = shell.execute(args);
  EXPECT_FALSE(result.success);
  EXPECT_NE(result.error.find("Too many arguments"), std::string::npos);
}

// ── WAL CRC32 Tests ─────────────────────────────────────────

TEST(WALIntegrity, CRC32RoundTrip) {
  std::filesystem::path test_dir = make_security_wal_test_dir("crc");
  std::filesystem::remove_all(test_dir);

  {
    memory::LocalGraphMemory graph(test_dir);
    (void)graph.add_node(memory::GraphNode{.id = "TestNode", .type = "Test", .content = ""});
    (void)graph.add_edge(memory::GraphEdge{
        .source_id = "TestNode", .target_id = "Other", .relation = "tests"});
  }

  // Re-open should load successfully with CRC verification
  {
    memory::LocalGraphMemory graph(test_dir);
    auto edges = graph.get_edges("TestNode");
    ASSERT_TRUE(edges.has_value());
    EXPECT_EQ(edges->size(), 1u);
  }

  std::filesystem::remove_all(test_dir);
}

TEST(WALIntegrity, CorruptedLineSkipped) {
  std::filesystem::path test_dir = make_security_wal_test_dir("corrupt");
  std::filesystem::remove_all(test_dir);

  // Write valid data first
  {
    memory::LocalGraphMemory graph(test_dir);
    (void)graph.add_node(memory::GraphNode{.id = "Good", .type = "Valid", .content = ""});
  }

  // Append a corrupted line to WAL
  {
    std::ofstream ofs(test_dir / "graph_wal.log", std::ios::app);
    ofs << "N,Corrupted,Data,Here,12345|CRC:99999999\n"; // Wrong CRC
  }

  // Re-open should skip the corrupted line but load the valid one
  {
    memory::LocalGraphMemory graph(test_dir);
    auto res = graph.k_hop_search("Good", 0);
    ASSERT_TRUE(res.has_value());
    EXPECT_EQ(res->nodes.size(), 1u);

    // Corrupted node should not exist
    auto res2 = graph.k_hop_search("Corrupted", 0);
    EXPECT_FALSE(res2.has_value()); // Not found
  }

  std::filesystem::remove_all(test_dir);
}
