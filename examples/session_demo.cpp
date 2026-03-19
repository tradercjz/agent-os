// ============================================================
// AgentOS Session Persistence Demo
// 演示会话的保存、列举、加载和恢复
// ============================================================
#include <agentos/agentos.hpp>
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

int main() {
    namespace fs = std::filesystem;
    auto snap_dir = fs::temp_directory_path() / "agentos_session_demo";
    if (fs::exists(snap_dir)) fs::remove_all(snap_dir);

    auto backend = std::make_unique<kernel::MockLLMBackend>("mock");
    AgentOS os(std::move(backend),
               AgentOS::Config::builder()
                   .scheduler_threads(1)
                   .snapshot_dir(snap_dir.string())
                   .build());

    // ── 1. 创建 Agent 并对话 ──
    section("1. 创建 Agent 并建立对话上下文");

    auto agent = os.create_agent(
        AgentConfig::builder()
            .name("session-bot")
            .role_prompt("你是一个记忆力超强的助手。")
            .build());

    agent->use(Middleware{
        .name = "logger",
        .before = nullptr,
        .after = nullptr,
    });

    // 模拟几轮对话（通过直接向 context 添加消息）
    os.ctx().append(agent->id(), kernel::Message::user("我叫小明"));
    os.ctx().append(agent->id(), kernel::Message::assistant("你好小明！有什么可以帮你的？"));
    os.ctx().append(agent->id(), kernel::Message::user("帮我记住我的生日是3月19日"));
    os.ctx().append(agent->id(), kernel::Message::assistant("好的，已记住：小明的生日是3月19日。"));

    auto& win = os.ctx().get_window(agent->id());
    info(("当前消息数: " + std::to_string(win.message_count())).c_str());
    ok("对话上下文已建立");

    // ── 2. 保存会话 ──
    section("2. 保存会话 (save_session)");

    std::string config_json = R"({"name":"session-bot","role_prompt":"你是一个记忆力超强的助手。"})";
    std::vector<std::string> mw_names = {"logger"};

    auto save_res = os.ctx().save_session(agent->id(), config_json, mw_names,
                                           R"({"purpose":"demo"})");
    if (save_res) {
        info(("保存路径: " + save_res->string()).c_str());
    }
    ok("会话已持久化到磁盘");

    // ── 3. 列举会话 ──
    section("3. 列举会话 (list_sessions)");

    // 多保存一个会话
    os.ctx().append(agent->id(), kernel::Message::user("今天天气怎么样？"));
    (void)os.ctx().save_session(agent->id(), config_json, mw_names);

    auto list_res = os.ctx().list_sessions(agent->id());
    if (list_res) {
        info(("找到 " + std::to_string(list_res->size()) + " 个会话:").c_str());
        for (const auto& sid : *list_res) {
            info(("  - " + sid).c_str());
        }
    }
    ok("会话列表获取成功");

    // ── 4. 加载会话 ──
    section("4. 加载会话 (load_session)");

    // 加载第一个会话
    auto session_id = (*list_res)[0];
    auto load_res = os.ctx().load_session(agent->id(), session_id);
    if (load_res) {
        auto& state = *load_res;
        info(("Agent ID: " + std::to_string(state.agent_id)).c_str());
        info(("Session ID: " + state.session_id).c_str());
        info(("Config: " + state.config_json).c_str());
        info(("Middleware 数量: " + std::to_string(state.middleware_names.size())).c_str());
        for (const auto& name : state.middleware_names) {
            info(("  - " + name).c_str());
        }
        info(("消息数: " + std::to_string(state.context.messages.size())).c_str());
        info(("Metadata: " + state.metadata_json).c_str());
    }
    ok("会话加载成功");

    // ── 5. 恢复会话 ──
    section("5. 恢复会话 (Resume workflow)");
    info("实际恢复流程:");
    info("  1. load_session() → 获取 SessionState");
    info("  2. create_agent(state.config) → 重建 Agent");
    info("  3. 应用程序根据 middleware_names 重新注册中间件");
    info("  4. restore(agent_id) → 恢复上下文窗口");
    info("  5. Agent 恢复到保存时的对话状态");

    // 演示恢复
    if (load_res) {
        info("恢复的对话内容:");
        for (const auto& msg : load_res->context.messages) {
            std::string role_str;
            switch (msg.role) {
                case kernel::Role::System:    role_str = "System"; break;
                case kernel::Role::User:      role_str = "User"; break;
                case kernel::Role::Assistant:  role_str = "Assistant"; break;
                default: role_str = "Other"; break;
            }
            std::string preview = msg.content.substr(0, 40);
            info(("  [" + role_str + "] " + preview).c_str());
        }
    }
    ok("会话恢复演示完成");

    // 清理
    if (fs::exists(snap_dir)) fs::remove_all(snap_dir);

    std::cout << "\n\033[1;32m🎉 Session Persistence Demo 完成！\033[0m\n\n";
    return 0;
}
