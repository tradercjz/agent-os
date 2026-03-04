// ============================================================
// AgentOS Graph Memory Demo
// 演示本体论图数据库 (Ontology + AI) 在 Agent 中的实践
// ============================================================
#include <agentos/agent.hpp>
#include <chrono>
#include <iostream>

using namespace agentos;

void ok(std::string_view msg) {
  std::cout << "\033[32m  ✓ " << msg << "\033[0m\n";
}
void info(std::string_view msg) {
  std::cout << "\033[34m  · " << msg << "\033[0m\n";
}
void section(std::string_view title) {
  std::cout << "\n\033[1;36m══════════════════════════════════════\033[0m\n  "
            << "\033[1;33m" << title << "\033[0m\n"
            << "\033[1;36m══════════════════════════════════════\033[0m\n";
}

class DemoGraphBackend : public kernel::MockLLMBackend {
public:
  DemoGraphBackend() : kernel::MockLLMBackend("mock-graph-model") {}

  Result<kernel::LLMResponse> complete(const kernel::LLMRequest &req) override {
    if (!req.messages.empty() && req.messages.back().content == "[继续]") {
      kernel::LLMResponse resp;
      if (req.messages.size() >= 2 &&
          req.messages[req.messages.size() - 2].role == kernel::Role::Tool) {
        if (req.messages[req.messages.size() - 2].name == "query_ontology") {
          resp.content = "通过查询知识图谱得知，苹果的创始人之一史蒂夫·乔布斯曾"
                         "经在里德学院（Reed College）就读。";
        } else {
          resp.content = "好的，我已经将这条信息结构化并存入了知识图谱。";
        }
      } else {
        resp.content = "完成。";
      }
      resp.finish_reason = "stop";
      resp.completion_tokens = 20;
      return resp;
    }

    // Find the last real user message
    std::string last_user_msg;
    for (auto it = req.messages.rbegin(); it != req.messages.rend(); ++it) {
      if (it->role == kernel::Role::User && it->content != "[继续]") {
        last_user_msg = it->content;
        break;
      }
    }

    // Mock execution
    if (last_user_msg.find("苹果公司的创始人是史蒂夫·乔布斯") !=
        std::string::npos) {
      kernel::LLMResponse resp;
      resp.tool_calls.push_back({
          .id = "call_mock1",
          .name = "extract_knowledge_graph",
          .args_json =
              R"({"subject": "Apple", "predicate": "founded_by", "object": "Steve Jobs"})",
      });
      resp.finish_reason = "tool_calls";
      return resp;
    }
    if (last_user_msg.find("史蒂夫·乔布斯曾经在里德学院上学") !=
        std::string::npos) {
      kernel::LLMResponse resp;
      resp.tool_calls.push_back({
          .id = "call_mock2",
          .name = "extract_knowledge_graph",
          .args_json =
              R"({"subject": "Steve Jobs", "predicate": "studied_at", "object": "Reed College"})",
      });
      resp.finish_reason = "tool_calls";
      return resp;
    }
    if (last_user_msg.find("我想知道苹果的CEO或者创始人在哪里上过学？") !=
        std::string::npos) {
      kernel::LLMResponse resp;
      resp.tool_calls.push_back({
          .id = "call_mock3",
          .name = "query_ontology",
          .args_json = R"({"entity": "Apple"})",
      });
      resp.finish_reason = "tool_calls";
      return resp;
    }

    return MockLLMBackend::complete(req);
  }
};

int main() {
  section("Phase 1: Knowledge Graph Extraction (Ontology Building)");

  auto backend = std::make_unique<DemoGraphBackend>();

  AgentOS::Config os_cfg{.scheduler_threads = 2,
                         .tpm_limit = 50000,
                         .ltm_dir = "/tmp/agentos_graph_demo"};

  AgentOS os(std::move(backend), std::move(os_cfg));

  auto agent = os.create_agent(AgentConfig{
      .name = "GraphAgent",
      .role_prompt = "你是具备知识图谱构建和逻辑推理能力的智能体。",
      .allowed_tools = {"extract_knowledge_graph", "query_ontology"}});

  info("User: 苹果公司的创始人是史蒂夫·乔布斯");
  auto res1 = agent->run("苹果公司的创始人是史蒂夫·乔布斯");
  if (res1) {
    ok("Agent Response: " + *res1);
  } else {
    std::cout << "\033[31m  Error: " << res1.error().message << "\033[0m\n";
  }

  info("User: 史蒂夫·乔布斯曾经在里德学院上学");
  auto res2 = agent->run("史蒂夫·乔布斯曾经在里德学院上学");
  if (res2) {
    ok("Agent Response: " + *res2);
  } else {
    std::cout << "\033[31m  Error: " << res2.error().message << "\033[0m\n";
  }

  section("Phase 2: Graph Retrieval (Multi-hop Reasoning)");
  info("User: 我想知道苹果的CEO或者创始人在哪里上过学？");
  auto res3 = agent->run("我想知道苹果的CEO或者创始人在哪里上过学？");

  if (res3) {
    ok("Agent Response: " + *res3);
  } else {
    std::cout << "\033[31m  Error: " << res3.error().message << "\033[0m\n";
  }

  // Check graph state directly for demo purposes
  section("Graph Database Dump");
  auto subgraph = os.memory().query_graph("Apple", 2);
  if (subgraph) {
    for (const auto &edge : subgraph->edges) {
      std::cout << "  (Node: " << edge.source_id << ") --[" << edge.relation
                << "]--> (Node: " << edge.target_id << ")\n";
    }
  }

  return 0;
}
