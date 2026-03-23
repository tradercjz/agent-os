// ============================================================
// AgentOS :: Extended RBAC Tests
// Custom roles, multi-permission, reassignment, concurrency
// ============================================================
#include <agentos/security/security.hpp>
#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

using namespace agentos;
using namespace agentos::security;

// ── 1. Custom role definition and assignment ──

TEST(RBACExtended, DefineAndAssignCustomRole) {
    RBAC rbac;
    rbac.define_role("analyst",
                     Permission::ToolReadOnly | Permission::MemoryRead);
    rbac.assign_role(100, "analyst");

    EXPECT_TRUE(rbac.may(100, Permission::ToolReadOnly));
    EXPECT_TRUE(rbac.may(100, Permission::MemoryRead));
    EXPECT_FALSE(rbac.may(100, Permission::ToolWrite));
    EXPECT_FALSE(rbac.may(100, Permission::MemoryWrite));
    EXPECT_FALSE(rbac.may(100, Permission::ToolDangerous));
}

TEST(RBACExtended, DefineCustomRoleWithMultiplePermissions) {
    RBAC rbac;
    Permission perms = Permission::ToolReadOnly | Permission::ToolWrite |
                       Permission::MemoryRead | Permission::MemoryWrite |
                       Permission::AgentCreate;
    rbac.define_role("developer", perms);
    rbac.assign_role(101, "developer");

    EXPECT_TRUE(rbac.may(101, Permission::ToolReadOnly));
    EXPECT_TRUE(rbac.may(101, Permission::ToolWrite));
    EXPECT_TRUE(rbac.may(101, Permission::MemoryRead));
    EXPECT_TRUE(rbac.may(101, Permission::MemoryWrite));
    EXPECT_TRUE(rbac.may(101, Permission::AgentCreate));
    EXPECT_FALSE(rbac.may(101, Permission::ToolDangerous));
    EXPECT_FALSE(rbac.may(101, Permission::AgentKill));
    EXPECT_FALSE(rbac.may(101, Permission::MemoryDelete));
}

TEST(RBACExtended, CustomRoleOverridesBuiltin) {
    RBAC rbac;
    // Redefine "readonly" with extra permissions
    rbac.define_role("readonly",
                     Permission::ToolReadOnly | Permission::MemoryRead |
                     Permission::ToolWrite);
    rbac.assign_role(102, "readonly");

    // Should now have ToolWrite, unlike the original readonly role
    EXPECT_TRUE(rbac.may(102, Permission::ToolWrite));
    EXPECT_TRUE(rbac.may(102, Permission::ToolReadOnly));
}

// ── 2. Multiple permission checks ──

TEST(RBACExtended, MultiplePermissionChecksOnSameAgent) {
    RBAC rbac;
    rbac.assign_role(200, "standard");

    // Check all expected permissions in sequence
    EXPECT_TRUE(rbac.may(200, Permission::ToolReadOnly));
    EXPECT_TRUE(rbac.may(200, Permission::ToolWrite));
    EXPECT_TRUE(rbac.may(200, Permission::MemoryRead));
    EXPECT_TRUE(rbac.may(200, Permission::MemoryWrite));
    EXPECT_TRUE(rbac.may(200, Permission::AgentCreate));
    EXPECT_TRUE(rbac.may(200, Permission::AgentObserve));

    // Check all denied permissions
    EXPECT_FALSE(rbac.may(200, Permission::ToolDangerous));
    EXPECT_FALSE(rbac.may(200, Permission::ToolSystem));
    EXPECT_FALSE(rbac.may(200, Permission::AgentKill));
    EXPECT_FALSE(rbac.may(200, Permission::MemoryDelete));
}

TEST(RBACExtended, MultipleAgentsDifferentRoles) {
    RBAC rbac;
    rbac.assign_role(201, "readonly");
    rbac.assign_role(202, "standard");
    rbac.assign_role(203, "privileged");
    rbac.assign_role(204, "admin");

    // readonly: no write
    EXPECT_FALSE(rbac.may(201, Permission::ToolWrite));
    // standard: write but no dangerous
    EXPECT_TRUE(rbac.may(202, Permission::ToolWrite));
    EXPECT_FALSE(rbac.may(202, Permission::ToolDangerous));
    // privileged: dangerous but no system
    EXPECT_TRUE(rbac.may(203, Permission::ToolDangerous));
    EXPECT_FALSE(rbac.may(203, Permission::ToolSystem));
    // admin: everything
    EXPECT_TRUE(rbac.may(204, Permission::ToolSystem));
    EXPECT_TRUE(rbac.may(204, Permission::ToolDangerous));
}

// ── 3. Role reassignment ──

TEST(RBACExtended, ReassignRoleUpgrade) {
    RBAC rbac;
    rbac.assign_role(300, "readonly");
    EXPECT_FALSE(rbac.may(300, Permission::ToolWrite));

    // Upgrade to standard
    rbac.assign_role(300, "standard");
    EXPECT_TRUE(rbac.may(300, Permission::ToolWrite));
    EXPECT_FALSE(rbac.may(300, Permission::ToolDangerous));

    // Upgrade to privileged
    rbac.assign_role(300, "privileged");
    EXPECT_TRUE(rbac.may(300, Permission::ToolDangerous));
}

TEST(RBACExtended, ReassignRoleDowngrade) {
    RBAC rbac;
    rbac.assign_role(301, "admin");
    EXPECT_TRUE(rbac.may(301, Permission::ToolDangerous));
    EXPECT_TRUE(rbac.may(301, Permission::ToolSystem));

    // Downgrade to readonly
    rbac.assign_role(301, "readonly");
    EXPECT_FALSE(rbac.may(301, Permission::ToolDangerous));
    EXPECT_FALSE(rbac.may(301, Permission::ToolWrite));
    EXPECT_TRUE(rbac.may(301, Permission::ToolReadOnly));
}

TEST(RBACExtended, ReassignToCustomRole) {
    RBAC rbac;
    rbac.assign_role(302, "admin");
    rbac.define_role("minimal", Permission::ToolReadOnly);
    rbac.assign_role(302, "minimal");

    EXPECT_TRUE(rbac.may(302, Permission::ToolReadOnly));
    EXPECT_FALSE(rbac.may(302, Permission::ToolWrite));
    EXPECT_FALSE(rbac.may(302, Permission::MemoryRead));
}

// ── 4. Unknown agent permission check ──

TEST(RBACExtended, UnknownAgentCheckReturnError) {
    RBAC rbac;
    auto result = rbac.check(999, Permission::ToolReadOnly);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::PermissionDenied);
}

TEST(RBACExtended, UnknownAgentMayReturnsFalse) {
    RBAC rbac;
    EXPECT_FALSE(rbac.may(999, Permission::ToolReadOnly));
    EXPECT_FALSE(rbac.may(999, Permission::Admin));
    EXPECT_FALSE(rbac.may(999, Permission::MemoryRead));
}

TEST(RBACExtended, AssignNonexistentRoleThenCheck) {
    RBAC rbac;
    rbac.assign_role(400, "nonexistent_role");

    auto result = rbac.check(400, Permission::ToolReadOnly);
    EXPECT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::PermissionDenied);

    EXPECT_FALSE(rbac.may(400, Permission::ToolReadOnly));
}

// ── 5. Admin role has all permissions ──

TEST(RBACExtended, AdminHasEveryPermission) {
    RBAC rbac;
    rbac.assign_role(500, "admin");

    // Exhaustively check every permission
    EXPECT_TRUE(rbac.may(500, Permission::ToolReadOnly));
    EXPECT_TRUE(rbac.may(500, Permission::ToolWrite));
    EXPECT_TRUE(rbac.may(500, Permission::ToolDangerous));
    EXPECT_TRUE(rbac.may(500, Permission::ToolSystem));
    EXPECT_TRUE(rbac.may(500, Permission::AgentCreate));
    EXPECT_TRUE(rbac.may(500, Permission::AgentKill));
    EXPECT_TRUE(rbac.may(500, Permission::AgentObserve));
    EXPECT_TRUE(rbac.may(500, Permission::MemoryRead));
    EXPECT_TRUE(rbac.may(500, Permission::MemoryWrite));
    EXPECT_TRUE(rbac.may(500, Permission::MemoryDelete));
    EXPECT_TRUE(rbac.may(500, Permission::Admin));
}

TEST(RBACExtended, AdminCheckReturnsSuccess) {
    RBAC rbac;
    rbac.assign_role(501, "admin");

    auto r = rbac.check(501, Permission::ToolDangerous);
    EXPECT_TRUE(r.has_value());

    auto r2 = rbac.check(501, Permission::ToolSystem);
    EXPECT_TRUE(r2.has_value());
}

// ── 6. may() vs check() behavior ──

TEST(RBACExtended, MayReturnsBoolCheckReturnsResult) {
    RBAC rbac;
    rbac.assign_role(600, "readonly");

    // may() returns bool
    bool can_read = rbac.may(600, Permission::ToolReadOnly);
    EXPECT_TRUE(can_read);

    bool can_write = rbac.may(600, Permission::ToolWrite);
    EXPECT_FALSE(can_write);

    // check() returns Result<void> with error details
    auto r_ok = rbac.check(600, Permission::ToolReadOnly);
    EXPECT_TRUE(r_ok.has_value());

    auto r_fail = rbac.check(600, Permission::ToolWrite);
    EXPECT_FALSE(r_fail.has_value());
    EXPECT_EQ(r_fail.error().code, ErrorCode::PermissionDenied);
    // Error message should contain agent ID and role name
    EXPECT_NE(r_fail.error().message.find("600"), std::string::npos);
    EXPECT_NE(r_fail.error().message.find("readonly"), std::string::npos);
}

TEST(RBACExtended, CheckErrorMessageForUnassignedAgent) {
    RBAC rbac;
    auto r = rbac.check(700, Permission::ToolReadOnly);
    EXPECT_FALSE(r.has_value());
    // Error message should mention "no role assigned"
    EXPECT_NE(r.error().message.find("no role"), std::string::npos);
}

TEST(RBACExtended, MayConsistentWithCheck) {
    RBAC rbac;
    rbac.assign_role(601, "standard");

    // may() and check() should agree for every permission
    std::vector<Permission> all_perms = {
        Permission::ToolReadOnly, Permission::ToolWrite,
        Permission::ToolDangerous, Permission::ToolSystem,
        Permission::AgentCreate, Permission::AgentKill,
        Permission::AgentObserve, Permission::MemoryRead,
        Permission::MemoryWrite, Permission::MemoryDelete,
    };

    for (auto p : all_perms) {
        bool may_result = rbac.may(601, p);
        auto check_result = rbac.check(601, p);
        EXPECT_EQ(may_result, check_result.has_value())
            << "Mismatch for permission " << static_cast<uint32_t>(p);
    }
}

// ── 7. Concurrent role assignment thread safety ──

TEST(RBACExtended, ConcurrentRoleAssignment) {
    RBAC rbac;
    constexpr int num_agents = 100;
    constexpr int num_threads = 4;

    // Multiple threads assign roles concurrently
    std::vector<std::thread> threads;
    for (int t = 0; t < num_threads; ++t) {
        threads.emplace_back([&rbac, t] {
            for (int i = t * (num_agents / num_threads);
                 i < (t + 1) * (num_agents / num_threads); ++i) {
                AgentId id = static_cast<AgentId>(i);
                rbac.assign_role(id, (i % 2 == 0) ? "readonly" : "standard");
            }
        });
    }
    for (auto& t : threads) t.join();

    // Verify all agents got assigned
    for (int i = 0; i < num_agents; ++i) {
        AgentId id = static_cast<AgentId>(i);
        auto r = rbac.check(id, Permission::ToolReadOnly);
        EXPECT_TRUE(r.has_value())
            << "Agent " << id << " should have ToolReadOnly";
    }
}

TEST(RBACExtended, ConcurrentCheckAndAssign) {
    RBAC rbac;
    // Pre-assign some agents
    for (int i = 0; i < 50; ++i) {
        rbac.assign_role(static_cast<AgentId>(i), "standard");
    }

    std::atomic<bool> stop{false};
    std::atomic<int> check_count{0};

    // Writer thread: reassign roles
    std::thread writer([&] {
        for (int round = 0; round < 50; ++round) {
            for (int i = 0; i < 50; ++i) {
                rbac.assign_role(static_cast<AgentId>(i),
                                 (round % 2 == 0) ? "readonly" : "privileged");
            }
        }
        stop.store(true);
    });

    // Reader threads: check permissions
    std::vector<std::thread> readers;
    for (int t = 0; t < 4; ++t) {
        readers.emplace_back([&] {
            int local_checks = 0;
            while (!stop.load() && local_checks < 500) {
                for (int i = 0; i < 50 && !stop.load(); ++i) {
                    // may() should never crash, regardless of concurrent writes
                    (void)rbac.may(static_cast<AgentId>(i), Permission::ToolReadOnly);
                    local_checks++;
                }
            }
            check_count.fetch_add(local_checks);
        });
    }

    writer.join();
    for (auto& r : readers) r.join();

    EXPECT_GT(check_count.load(), 0);
}

TEST(RBACExtended, ConcurrentDefineAndUseCustomRoles) {
    RBAC rbac;
    std::atomic<bool> stop{false};

    // Thread 1: define custom roles
    std::thread definer([&] {
        for (int i = 0; i < 100; ++i) {
            rbac.define_role("custom_" + std::to_string(i),
                             Permission::ToolReadOnly | Permission::MemoryRead);
        }
        stop.store(true);
    });

    // Thread 2: assign those roles to agents
    std::thread assigner([&] {
        for (int i = 0; i < 100 && !stop.load(); ++i) {
            rbac.assign_role(static_cast<AgentId>(i + 1000),
                             "custom_" + std::to_string(i % 50));
        }
    });

    // Thread 3: check permissions
    std::thread checker([&] {
        int count = 0;
        while (!stop.load() && count < 500) {
            (void)rbac.may(static_cast<AgentId>(1000), Permission::ToolReadOnly);
            count++;
        }
    });

    definer.join();
    assigner.join();
    checker.join();

    // If we reach here without deadlock/crash, concurrency is safe
    SUCCEED();
}

// ── SecurityManager integration with RBAC ──

TEST(RBACExtended, SecurityManagerGrantAndMay) {
    SecurityManager sec;
    sec.grant(800, "standard");

    EXPECT_TRUE(sec.may(800, Permission::ToolReadOnly));
    EXPECT_TRUE(sec.may(800, Permission::ToolWrite));
    EXPECT_FALSE(sec.may(800, Permission::ToolDangerous));

    // Reassign via grant
    sec.grant(800, "admin");
    EXPECT_TRUE(sec.may(800, Permission::ToolDangerous));
    EXPECT_TRUE(sec.may(800, Permission::Admin));
}

TEST(RBACExtended, SecurityManagerAuthorizeTool) {
    SecurityManager sec;
    sec.grant(801, "readonly");

    // Readonly should be allowed to call a read-only tool
    auto r1 = sec.authorize_tool(801, "http_fetch", "{}");
    EXPECT_TRUE(r1.has_value());

    // Readonly should be denied a write tool (kv_store is marked write)
    auto r2 = sec.authorize_tool(801, "kv_store", "{}");
    EXPECT_FALSE(r2.has_value());
}
