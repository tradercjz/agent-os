// ============================================================
// AgentOS — OpenAI / 兼容 API 接入示例
//
// 使用方法：
//   export OPENAI_API_KEY="sk-..."
//   export OPENAI_BASE_URL="https://api.openai.com/v1"   # 可选，默认 OpenAI
//   export OPENAI_MODEL="gpt-4o-mini"                    # 可选，默认
//   gpt-4o-mini
//   ./build/openai_demo
//
// 兼容 API（任意 OpenAI-compatible 服务只需修改 OPENAI_BASE_URL）：
//   Ollama：  OPENAI_BASE_URL=http://localhost:11434/v1  OPENAI_MODEL=llama3
//   硅基流动：OPENAI_BASE_URL=https://api.siliconflow.cn/v1
//   OPENAI_MODEL=Qwen/Qwen2.5-7B-Instruct 智谱 AI：
//   OPENAI_BASE_URL=https://open.bigmodel.cn/api/paas/v4
//   OPENAI_MODEL=glm-4-flash
// ============================================================
#include <agentos/agent.hpp>
#include <cstdlib>
#include <iostream>
#include <string>

using namespace agentos;
using namespace std::chrono_literals;

// ── 读取环境变量（带默认值）────────────────────────────────
static std::string env(const char *key, std::string def = "") {
  const char *v = std::getenv(key);
  return v ? std::string(v) : def;
}

// ── 彩色打印工具 ─────────────────────────────────────────────
static void print_banner() {
  std::cout << "\033[1;35m"
            << "  ╔══════════════════════════════════════╗\n"
            << "  ║   AgentOS × OpenAI  Interactive Demo  ║\n"
            << "  ╚══════════════════════════════════════╝\n"
            << "\033[0m\n";
}

static void print_info(const std::string &msg) {
  std::cout << "\033[34m[INFO] " << msg << "\033[0m\n";
}
static void print_err(const std::string &msg) {
  std::cout << "\033[31m[ERR ] " << msg << "\033[0m\n";
}
static void print_tool(const std::string &name, const std::string &result) {
  std::cout << "\033[33m[工具:" << name << "] " << result << "\033[0m\n";
}

// ── 自定义 Agent：打印工具调用过程 ──────────────────────────
class VerboseReActAgent : public Agent {
public:
  VerboseReActAgent(AgentId id, AgentConfig cfg, AgentOS *os)
      : Agent(id, std::move(cfg), os) {}

  Result<std::string> run(std::string user_input) override {
    // 从记忆中检索相关上下文
    if (auto memories = recall(user_input, 3)) {
      if (!memories->empty()) {
        std::string mem_ctx = "历史记忆：\n";
        for (auto &sr : *memories)
          mem_ctx += "- " + sr.entry.content + "\n";
        os_->ctx().append(id_, kernel::Message::system(mem_ctx));
      }
    }

    for (int step = 0; step < MAX_STEPS; ++step) {
      // ── Think ──────────────────────────────────────────
      std::cout << "\033[32m助手: \033[0m" << std::flush;
      auto resp =
          think(step == 0 ? user_input : "[继续]", [](std::string_view token) {
            std::cout << "\033[32m" << token << "\033[0m" << std::flush;
          });
      std::cout << "\n";
      if (!resp) {
        print_err("LLM 推理失败: " + resp.error().message);
        return make_unexpected(resp.error());
      }

      // ── 无工具调用：直接返回 ───────────────────────────
      if (!resp->wants_tool_call()) {
        remember(fmt::format("Q: {} → A: {}", user_input, resp->content), 0.6f);
        return resp->content;
      }

      // ── Act：执行工具调用 ──────────────────────────────
      for (auto &tc : resp->tool_calls) {
        std::cout << "\033[33m  → 调用工具 [" << tc.name << "] " << tc.args_json
                  << "\033[0m\n";

        auto tool_result = act(tc);
        std::string obs;
        if (tool_result) {
          obs = tool_result->success ? tool_result->output
                                     : "工具执行失败: " + tool_result->error;
          print_tool(tc.name, obs);
        } else {
          obs = "工具调用被拒绝: " + tool_result.error().message;
          print_err(obs);
        }

        // Observe：追加工具结果到上下文
        kernel::Message obs_msg;
        obs_msg.role = kernel::Role::Tool;
        obs_msg.content = obs;
        obs_msg.tool_call_id = tc.id;
        obs_msg.name = tc.name;
        os_->ctx().append(id_, obs_msg);
      }
    }

    return make_error(ErrorCode::Unknown, "超出最大推理步数");
  }

private:
  static constexpr int MAX_STEPS = 10;
};

// ─────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────
int main() {
  print_banner();

  // ── 1. 读取配置 ─────────────────────────────────────────
  std::string api_key = env("OPENAI_API_KEY");
  std::string base_url = env("OPENAI_BASE_URL", "https://api.openai.com/v1");
  std::string model = env("OPENAI_MODEL", "gpt-4o-mini");

  if (api_key.empty()) {
    print_err("请先设置环境变量 OPENAI_API_KEY");
    print_err("  export OPENAI_API_KEY=\"sk-...\"");
    return 1;
  }

  print_info(fmt::format("后端: {}  模型: {}", base_url, model));

  // ── 2. 构建 AgentOS ─────────────────────────────────────
  auto backend =
      std::make_unique<kernel::OpenAIBackend>(api_key, base_url, model);

  AgentOS::Config os_cfg;
  os_cfg.tpm_limit = 200000;
  os_cfg.scheduler_threads = 2;
  os_cfg.snapshot_dir = "/tmp/agentos_openai_snap";
  os_cfg.ltm_dir = "/tmp/agentos_openai_ltm";
  os_cfg.enable_security = true;

  AgentOS os(std::move(backend), os_cfg);

  // ── 3. 注册自定义工具 ────────────────────────────────────
  os.tools().registry().register_fn(
      tools::ToolSchema{
          .id = "calculator",
          .description = "执行数学计算，支持 +、-、*、/ 运算",
          .params =
              {
                  {"expression", tools::ParamType::String,
                   "数学表达式，如 \"(10 + 5) * 3\""},
              },
      },
      [](const tools::ParsedArgs &args) -> tools::ToolResult {
        // 调用系统 bc 进行计算（安全：仅传表达式，不执行 shell）
        std::string expr = args.get("expression");
        // 简单白名单检测
        for (char c : expr) {
          if (!std::isdigit(c) && c != '+' && c != '-' && c != '*' &&
              c != '/' && c != '(' && c != ')' && c != ' ' && c != '.') {
            return tools::ToolResult::fail("表达式包含非法字符");
          }
        }
        std::string cmd = "echo \"" + expr + "\" | bc 2>&1";
        char buf[256] = {};
        FILE *p = popen(cmd.c_str(), "r");
        if (!p)
          return tools::ToolResult::fail("bc 调用失败");
        fgets(buf, sizeof(buf), p);
        pclose(p);
        std::string result(buf);
        while (!result.empty() &&
               (result.back() == '\n' || result.back() == '\r'))
          result.pop_back();
        return tools::ToolResult::ok(result);
      });

  print_info(fmt::format(
      "已注册 {} 个工具: kv_store, shell_exec, http_fetch, calculator",
      os.tools().registry().list_schemas().size()));

  // ── 4. 创建 Agent ────────────────────────────────────────
  AgentConfig agent_cfg;
  agent_cfg.name = "Assistant";
  agent_cfg.role_prompt = "你是一个有用的 AI 助手，由 AgentOS 驱动。"
                          "你可以使用以下工具帮助用户：\n"
                          "- kv_store：存储和读取键值数据\n"
                          "- calculator：执行数学计算\n"
                          "- shell_exec：执行安全的 shell 命令（仅限白名单）\n"
                          "请用中文回答，简洁准确。";
  agent_cfg.security_role = "standard";
  agent_cfg.context_limit = 8192;
  // 不限制 allowed_tools，让 Agent 自行选择
  agent_cfg.allowed_tools = {"kv_store", "calculator", "shell_exec"};
  agent_cfg.persist_memory = true;

  auto agent = os.create_agent<VerboseReActAgent>(agent_cfg);
  print_info(fmt::format("Agent 已启动 (id={})", agent->id()));

  // ── 5. 交互循环 ──────────────────────────────────────────
  std::cout << "\n输入消息与 Agent 对话，输入 \033[1mquit\033[0m 退出，"
            << "\033[1mstatus\033[0m 查看系统状态。\n\n";

  std::string input;
  while (true) {
    std::cout << "\033[1;37m你: \033[0m";
    if (!std::getline(std::cin, input))
      break;

    if (input == "quit" || input == "exit") {
      std::cout << "再见！\n";
      break;
    }
    if (input == "status") {
      std::cout << os.status() << "\n";
      continue;
    }
    if (input.empty())
      continue;

    std::cout << "\033[90m  [思考中...]\033[0m\n";
    auto result = agent->run(input);

    if (!result) {
      print_err("错误: " + result.error().message);
    }
    std::cout << "\n";
  }

  // ── 6. 退出前快照上下文 ──────────────────────────────────
  os.ctx().snapshot(agent->id(), R"({"session":"openai_demo"})");
  print_info("上下文已保存。");

  return 0;
}
