// ============================================================
// AgentOS Worktree Isolation Demo
// 演示 WorktreeManager 的 CRUD、变更检测与崩溃恢复
// ============================================================
#include <agentos/worktree/worktree_manager.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace fs = std::filesystem;
using namespace agentos::worktree;

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
    // 使用当前仓库作为演示目录
    auto repo_root = fs::current_path();
    auto wt_base = repo_root / "build" / "demo_worktrees";

    // ── 1. 创建 WorktreeManager ──
    section("1. 创建 WorktreeManager");

    WorktreeConfig cfg{
        .repo_root = repo_root,
        .worktree_base = wt_base,
        .max_concurrent = 3,        // 最多 3 个并发 worktree
        .auto_cleanup = true,
    };
    WorktreeManager mgr(cfg);

    info(("worktree 基目录: " + wt_base.string()).c_str());
    ok("WorktreeManager 初始化完成");

    // ── 2. 创建 Worktree ──
    section("2. 创建 Worktree");

    auto res = mgr.create("feature-search");
    if (!res) {
        std::cerr << "创建失败: " << res.error().message << "\n";
        return 1;
    }
    auto& wt = res.value();

    info(("名称:   " + wt.name).c_str());
    info(("分支:   " + wt.branch).c_str());
    info(("路径:   " + wt.path.string()).c_str());
    ok("Worktree 已创建，agent 可在此目录独立工作");

    // ── 3. 查询状态 ──
    section("3. 查询状态");

    info(("当前活跃数: " + std::to_string(mgr.active_count())).c_str());
    info(("已达容量上限: " + std::string(mgr.at_capacity() ? "是" : "否")).c_str());

    auto got = mgr.get("feature-search");
    if (got) {
        info(("get() 返回分支: " + got->branch).c_str());
    }

    auto listed = mgr.list();
    if (listed) {
        info(("list() 返回 " + std::to_string(listed->size()) + " 个 worktree").c_str());
    }
    ok("查询接口正常");

    // ── 4. 变更检测 ──
    section("4. 变更检测 (has_changes)");

    auto clean = mgr.has_changes("feature-search");
    if (clean) {
        info(("修改前: has_changes = " + std::string(*clean ? "true" : "false")).c_str());
    }

    // 在 worktree 中创建一个文件，模拟 agent 的代码修改
    std::ofstream(wt.path / "agent_output.txt") << "Agent generated code here";

    auto dirty = mgr.has_changes("feature-search");
    if (dirty) {
        info(("修改后: has_changes = " + std::string(*dirty ? "true" : "false")).c_str());
    }
    ok("变更检测正常工作");

    // ── 5. 容量限制 ──
    section("5. 容量限制");

    (void)mgr.create("feature-index");
    (void)mgr.create("feature-cache");
    info(("已创建 " + std::to_string(mgr.active_count()) + "/3 个 worktree").c_str());

    auto overflow = mgr.create("feature-overflow");
    if (!overflow) {
        info(("第 4 个被拒绝: " + overflow.error().message).c_str());
    }
    ok("容量限制生效");

    // ── 6. 崩溃恢复 ──
    section("6. 崩溃恢复 (recover)");

    // 模拟进程重启：用新的 manager（无内存状态）恢复
    WorktreeManager mgr2(cfg);
    info(("重启后 active_count = " + std::to_string(mgr2.active_count())).c_str());

    auto rec = mgr2.recover();
    if (rec) {
        info(("恢复后 active_count = " + std::to_string(mgr2.active_count())).c_str());
    }
    ok("崩溃恢复完成，孤立 worktree 已重新纳管");

    // ── 7. 清理 ──
    section("7. 清理");

    // 用恢复后的 manager 清理所有 worktree
    auto all = mgr2.list();
    if (all) {
        for (const auto& w : *all) {
            auto rm = mgr2.remove(w.name, /*force=*/true);
            if (rm) {
                info(("已删除: " + w.name).c_str());
            }
        }
    }
    // 也清理原 manager 可能残留的
    auto remaining = mgr.list();
    if (remaining) {
        for (const auto& w : *remaining) {
            (void)mgr.remove(w.name, true);
        }
    }
    if (fs::exists(wt_base)) {
        fs::remove_all(wt_base);
    }
    ok("全部清理完毕");

    std::cout << "\n\033[1;32m🎉 Worktree Isolation Demo 完成！\033[0m\n\n";
    return 0;
}
