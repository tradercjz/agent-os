// ============================================================
// AgentOS × Corax — Demo: 自然语言驱动实时流计算
//
// 完整端到端链路:
//   用户: "监控茅台，跌超1%通知我"
//   → Agent 自动: 搜索股票代码 → 拉取行情 → 创建 pipeline → 喂数据 → 检查告警
//
// 编译:
//   cmake --build build --target corax_demo
//
// 运行 (自定义 LLM endpoint):
//   ./build/corax_demo /path/to/bin/corax \
//     --api-key YOUR_KEY --api-base https://your-endpoint/v1 --model your-model
//
// 运行 (环境变量):
//   OPENAI_API_KEY=sk-... OPENAI_API_BASE=https://your-endpoint/v1 TUSHARE_TOKEN=xxx \
//     ./build/corax_demo /path/to/bin/corax
//
// 运行 (Server 模式 — 持久监控):
//   /path/to/bin/corax-server --port 8080 &
//   ./build/corax_demo /path/to/bin/corax --server http://localhost:8080 \
//     --api-key YOUR_KEY --api-base https://your-endpoint/v1 --model your-model
// ============================================================

#include <agentos/agentos.hpp>
#include <agentos/tools/corax_tools.hpp>
#include <agentos/tools/corax_server_tools.hpp>
#include <agentos/tools/corax_data_tools.hpp>
#include <agentos/tools/corax_prompt.hpp>

#include <filesystem>
#include <iostream>
#include <string>

using namespace agentos;

int main(int argc, char* argv[]) {
    // ── 1. 解析参数 ──
    std::string corax_binary = "bin/corax";
    std::string server_url;
    std::string fetch_script;
    std::string llm_api_base;
    std::string llm_api_key;
    std::string llm_model;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--server" && i + 1 < argc) {
            server_url = argv[++i];
        } else if (arg == "--fetch-script" && i + 1 < argc) {
            fetch_script = argv[++i];
        } else if (arg == "--api-base" && i + 1 < argc) {
            llm_api_base = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            llm_api_key = argv[++i];
        } else if (arg == "--model" && i + 1 < argc) {
            llm_model = argv[++i];
        } else if (arg[0] != '-') {
            corax_binary = arg;
        }
    }

    bool server_mode = !server_url.empty();

    // Auto-detect fetch_stock.py location
    if (fetch_script.empty()) {
        // Try relative to binary
        auto bin_dir = std::filesystem::path(argv[0]).parent_path();
        auto candidates = {
            bin_dir / "../tools/fetch_stock.py",
            bin_dir / "../../tools/fetch_stock.py",
            std::filesystem::path("tools/fetch_stock.py"),
        };
        for (const auto& p : candidates) {
            if (std::filesystem::exists(p)) {
                fetch_script = std::filesystem::canonical(p).string();
                break;
            }
        }
    }

    std::cout << "╔══════════════════════════════════════════════════════╗\n"
              << "║       Corax × AgentOS — 说人话，算行情              ║\n"
              << "╠══════════════════════════════════════════════════════╣\n"
              << "║  Corax binary : " << corax_binary << "\n";
    if (server_mode) {
        std::cout
              << "║  Server URL   : " << server_url << "\n"
              << "║  模式         : Server（持久 pipeline + 实时监控）  ║\n";
    } else {
        std::cout
              << "║  模式         : CLI（一次性查询）                   ║\n";
    }
    if (!fetch_script.empty()) {
        std::cout
              << "║  数据源       : tushare (via " << fetch_script << ")\n";
    }
    std::cout << "║                                                      ║\n"
              << "║  输入自然语言，Agent 自动调用流计算引擎              ║\n"
              << "║  输入 'quit' 退出                                    ║\n"
              << "╚══════════════════════════════════════════════════════╝\n\n";

    // ── 2. 选择 LLM Backend ──
    // 优先级: 命令行参数 > 环境变量
    if (llm_api_key.empty()) {
        if (auto env = std::getenv("OPENAI_API_KEY"); env && *env)
            llm_api_key = env;
        else if (auto env = std::getenv("ANTHROPIC_API_KEY"); env && *env)
            llm_api_key = env;
    }
    if (llm_api_base.empty()) {
        if (auto env = std::getenv("OPENAI_API_BASE"); env && *env)
            llm_api_base = env;
    }

    if (llm_api_key.empty()) {
        std::cerr << "[!] 请提供 API key:\n"
                  << "    --api-key YOUR_KEY --api-base https://your-endpoint/v1 --model your-model\n"
                  << "    或设置环境变量 OPENAI_API_KEY / OPENAI_API_BASE\n";
        return 1;
    }

    std::unique_ptr<kernel::ILLMBackend> backend;

    // 统一走 OpenAI-compatible 接口（支持任意 base_url）
    std::string api_base = llm_api_base.empty() ? "https://api.openai.com/v1" : llm_api_base;
    std::string model = llm_model.empty() ? "gpt-4o" : llm_model;

    std::cout << "[*] LLM Backend: " << api_base << " (model: " << model << ")\n";
    backend = std::make_unique<kernel::OpenAIBackend>(llm_api_key, api_base, model);

    // ── 3. 构建 AgentOS ──
    auto os = std::make_unique<AgentOS>(std::move(backend),
        AgentOS::Config::builder()
            .scheduler_threads(2)
            .tpm_limit(200000)
            .build());

    // ── 4. 注册工具 ──
    // CLI 工具（一次性查询）
    corax::register_corax_tools(os->tools().registry(), corax_binary);
    int tool_count = 10;

    // Server 工具（持久 pipeline）
    if (server_mode) {
        corax::register_corax_server_tools(os->tools().registry(), server_url);
        tool_count += 11;
    }

    // 数据源工具（tushare）
    if (!fetch_script.empty()) {
        corax::register_corax_data_tools(os->tools().registry(), fetch_script);
        tool_count += 2;
    }

    std::cout << "[*] 已注册 " << tool_count << " 个工具\n";

    // ── 5. 组装 allowed_tools 列表 ──
    auto tool_ids = corax::corax_tool_ids();

    if (server_mode) {
        auto srv = corax::corax_server_tool_ids();
        tool_ids.insert(tool_ids.end(), srv.begin(), srv.end());
    }

    if (!fetch_script.empty()) {
        auto data = corax::corax_data_tool_ids();
        tool_ids.insert(tool_ids.end(), data.begin(), data.end());
    }

    // ── 6. 创建 Agent ──
    auto agent = os->create_agent(
        AgentConfig::builder()
            .name("corax-agent")
            .role_prompt(std::string(corax::CORAX_SYSTEM_PROMPT))
            .tools(tool_ids)
            .context_limit(16384)
            .build());

    std::cout << "[*] Corax Agent 就绪 (ID: " << agent->id() << ")\n\n";

    if (!fetch_script.empty() && server_mode) {
        std::cout << "💡 试试说: \"监控茅台，跌超1%通知我\"\n"
                  << "   Agent 会自动: 搜索代码 → 拉取数据 → 创建 pipeline → 喂数据 → 检查告警\n\n";
    } else if (!fetch_script.empty()) {
        std::cout << "💡 试试说: \"查一下茅台最近30天的日线数据，找出涨幅最大的5天\"\n\n";
    }

    // ── 7. 交互循环 ──
    std::string input;
    while (true) {
        std::cout << "\033[1;36m你: \033[0m";
        if (!std::getline(std::cin, input)) break;

        if (input.empty()) continue;
        if (input == "quit" || input == "exit" || input == "q") {
            std::cout << "\n[*] 再见！\n";
            break;
        }

        std::cout << "\033[1;33m⏳ 思考中...\033[0m\n";

        auto result = agent->run(input);

        if (result) {
            std::cout << "\033[1;32mCorax: \033[0m" << *result << "\n\n";
        } else {
            std::cout << "\033[1;31m[错误] \033[0m" << result.error().message << "\n\n";
        }
    }

    os->destroy_agent(agent->id());
    return 0;
}
