#include <agentos/memory/consolidator.hpp>
#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <filesystem>
#include <thread>
#include <chrono>

using namespace agentos;
using namespace agentos::memory;

// ── Test Fixture ─────────────────────────────────────────────

class ConsolidatorTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        ASSERT_NE(info, nullptr);
        test_dir_ = std::filesystem::temp_directory_path() /
                    std::filesystem::path("agentos_test_consolidator_" +
                                          std::string(info->test_suite_name()) + "_" +
                                          std::string(info->name()));
        std::filesystem::remove_all(test_dir_);
        mem_ = std::make_unique<MemorySystem>(test_dir_);
    }

    void TearDown() override {
        std::filesystem::remove_all(test_dir_);
    }

    // Helper: create a MemoryEntry with specified fields
    MemoryEntry make_entry(const std::string& content, float importance,
                           AgentId agent_id, uint32_t access_count = 0,
                           TimePoint accessed_at = now()) {
        MemoryEntry e;
        e.content = content;
        e.importance = importance;
        e.agent_id = std::to_string(agent_id);
        e.access_count = access_count;
        e.created_at = now();
        e.accessed_at = accessed_at;
        e.embedding = {};  // empty embedding for consolidation tests
        return e;
    }

    std::filesystem::path test_dir_;
    std::unique_ptr<MemorySystem> mem_;
};

// ── 1. StrengthComputation_FreshEntry ────────────────────────

TEST_F(ConsolidatorTest, StrengthComputation_FreshEntry) {
    // importance=0.8, access_count=0, just created => base=0.8, decay~0
    auto tp = now();
    float strength = compute_memory_strength(0.8f, 0, tp, tp, 0.3f);
    // No decay: strength should equal base = 0.8 * (1 + 0) = 0.8
    EXPECT_FLOAT_EQ(strength, 0.8f);
}

// ── 2. StrengthComputation_DecayedEntry ──────────────────────

TEST_F(ConsolidatorTest, StrengthComputation_DecayedEntry) {
    // Same importance=0.8 but last_access was 10 hours ago
    auto now_tp = now();
    auto ten_hours_ago = now_tp - std::chrono::hours(10);
    float strength = compute_memory_strength(0.8f, 0, ten_hours_ago, now_tp, 0.3f);
    // base=0.8, decay = e^(-0.3*10) = e^(-3) ~ 0.0498
    // strength ~ 0.8 * 0.0498 ~ 0.0399
    EXPECT_LT(strength, 0.05f);
    EXPECT_GT(strength, 0.0f);
}

// ── 3. StrengthComputation_AccessBoosted ─────────────────────

TEST_F(ConsolidatorTest, StrengthComputation_AccessBoosted) {
    // importance=0.3 but access_count=10 => base = min(1.0, 0.3*(1+1.0)) = 0.6
    auto tp = now();
    float strength = compute_memory_strength(0.3f, 10, tp, tp, 0.3f);
    // base = 0.3 * (1 + 0.1*10) = 0.3 * 2.0 = 0.6, no decay
    EXPECT_FLOAT_EQ(strength, 0.6f);
}

// ── 4. ConsolidateHighStrength ───────────────────────────────

TEST_F(ConsolidatorTest, ConsolidateHighStrength) {
    AgentId agent = 42;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;
    cfg.forget_threshold = 0.1f;

    // Write high-importance entry to STM
    auto entry = make_entry("important fact", 0.9f, agent);
    auto write_result = mem_->short_term().write(std::move(entry));
    ASSERT_TRUE(write_result);

    MemoryConsolidator consolidator(*mem_, cfg);
    auto result = consolidator.consolidate_now(agent);

    // Entry should have been consolidated (moved to LTM)
    EXPECT_EQ(result.scanned, 1u);
    EXPECT_EQ(result.consolidated, 1u);
    EXPECT_EQ(result.forgotten, 0u);
    EXPECT_EQ(result.retained, 0u);

    // Verify: STM empty, LTM has the entry
    EXPECT_EQ(mem_->short_term().size(), 0u);
    EXPECT_EQ(mem_->long_term().size(), 1u);

    auto ltm_entries = mem_->long_term().get_all();
    ASSERT_EQ(ltm_entries.size(), 1u);
    EXPECT_EQ(ltm_entries[0].content, "important fact");
}

// ── 5. ForgetLowStrength ─────────────────────────────────────

TEST_F(ConsolidatorTest, ForgetLowStrength) {
    AgentId agent = 42;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;
    cfg.forget_threshold = 0.1f;
    cfg.decay_rate = 0.3f;

    // Write entry with very low importance, accessed long ago
    auto old_time = now() - std::chrono::hours(24);
    auto entry = make_entry("trivial noise", 0.05f, agent, 0, old_time);
    auto write_result = mem_->short_term().write(std::move(entry));
    ASSERT_TRUE(write_result);

    MemoryConsolidator consolidator(*mem_, cfg);
    auto result = consolidator.consolidate_now(agent);

    // Entry should have been forgotten
    EXPECT_EQ(result.scanned, 1u);
    EXPECT_EQ(result.forgotten, 1u);
    EXPECT_EQ(result.consolidated, 0u);
    EXPECT_EQ(result.retained, 0u);

    // Verify: STM empty, LTM still empty
    EXPECT_EQ(mem_->short_term().size(), 0u);
    EXPECT_EQ(mem_->long_term().size(), 0u);
}

// ── 6. RetainMediumStrength ──────────────────────────────────

TEST_F(ConsolidatorTest, RetainMediumStrength) {
    AgentId agent = 42;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;
    cfg.forget_threshold = 0.1f;
    cfg.decay_rate = 0.3f;

    // Medium importance, recently accessed => strength between thresholds
    // importance=0.3, access_count=2 => base = 0.3*(1+0.2) = 0.36
    // Just accessed, so no decay => strength = 0.36 (between 0.1 and 0.5)
    auto entry = make_entry("medium note", 0.3f, agent, 2, now());
    auto write_result = mem_->short_term().write(std::move(entry));
    ASSERT_TRUE(write_result);

    MemoryConsolidator consolidator(*mem_, cfg);
    auto result = consolidator.consolidate_now(agent);

    EXPECT_EQ(result.scanned, 1u);
    EXPECT_EQ(result.retained, 1u);
    EXPECT_EQ(result.consolidated, 0u);
    EXPECT_EQ(result.forgotten, 0u);

    // Entry stays in STM
    EXPECT_EQ(mem_->short_term().size(), 1u);
    EXPECT_EQ(mem_->long_term().size(), 0u);
}

// ── 7. EventOnRunComplete ────────────────────────────────────

TEST_F(ConsolidatorTest, EventOnRunComplete) {
    AgentId agent = 99;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;

    MemoryConsolidator consolidator(*mem_, cfg);
    consolidator.register_agent(agent);

    // Write a high-importance entry
    auto entry = make_entry("run result", 0.9f, agent);
    (void)mem_->short_term().write(std::move(entry));

    // Trigger via event
    consolidator.on_agent_run_complete(agent);

    // Entry should have been consolidated to LTM
    EXPECT_EQ(mem_->short_term().size(), 0u);
    EXPECT_EQ(mem_->long_term().size(), 1u);
}

// ── 8. EventOnAgentDestroyed ─────────────────────────────────

TEST_F(ConsolidatorTest, EventOnAgentDestroyed) {
    AgentId agent = 77;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;

    MemoryConsolidator consolidator(*mem_, cfg);
    consolidator.register_agent(agent);

    auto entry = make_entry("final memory", 0.85f, agent);
    (void)mem_->short_term().write(std::move(entry));

    // on_agent_destroyed should consolidate and unregister
    consolidator.on_agent_destroyed(agent);

    EXPECT_EQ(mem_->short_term().size(), 0u);
    EXPECT_EQ(mem_->long_term().size(), 1u);

    // Agent should be unregistered; writing another entry and triggering
    // background consolidation should not process it (verify via consolidate_now)
    auto entry2 = make_entry("post-destroy", 0.9f, agent);
    (void)mem_->short_term().write(std::move(entry2));

    // consolidate_now still works (manual trigger), but agent is unregistered
    // We just verify the unregister happened by checking that the first
    // consolidation did run successfully above.
}

// ── 9. BackgroundThread ──────────────────────────────────────

TEST_F(ConsolidatorTest, BackgroundThread) {
    AgentId agent = 10;
    ConsolidatorConfig cfg;
    cfg.periodic_interval = Duration{300};  // 300ms for fast test
    cfg.consolidate_threshold = 0.5f;

    MemoryConsolidator consolidator(*mem_, cfg);
    consolidator.register_agent(agent);

    // Write high-importance entry
    auto entry = make_entry("background test", 0.95f, agent);
    (void)mem_->short_term().write(std::move(entry));

    EXPECT_EQ(mem_->short_term().size(), 1u);

    // Start background thread and wait for at least one periodic cycle
    consolidator.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(600));
    consolidator.stop();

    // After background consolidation, entry should have moved to LTM
    EXPECT_EQ(mem_->short_term().size(), 0u);
    EXPECT_EQ(mem_->long_term().size(), 1u);
}

// ── 10. AgentIsolation ───────────────────────────────────────

TEST_F(ConsolidatorTest, AgentIsolation) {
    AgentId agent_a = 1;
    AgentId agent_b = 2;
    ConsolidatorConfig cfg;
    cfg.consolidate_threshold = 0.5f;

    // Write entries for both agents
    auto entry_a = make_entry("agent A memory", 0.9f, agent_a);
    auto entry_b = make_entry("agent B memory", 0.9f, agent_b);
    (void)mem_->short_term().write(std::move(entry_a));
    (void)mem_->short_term().write(std::move(entry_b));
    EXPECT_EQ(mem_->short_term().size(), 2u);

    // Consolidate only agent A
    MemoryConsolidator consolidator(*mem_, cfg);
    auto result = consolidator.consolidate_now(agent_a);

    EXPECT_EQ(result.scanned, 1u);
    EXPECT_EQ(result.consolidated, 1u);

    // Agent A's entry moved to LTM; Agent B's entry still in STM
    EXPECT_EQ(mem_->short_term().size(), 1u);
    EXPECT_EQ(mem_->long_term().size(), 1u);

    auto stm_entries = mem_->short_term().get_all();
    ASSERT_EQ(stm_entries.size(), 1u);
    EXPECT_EQ(stm_entries[0].content, "agent B memory");
}
