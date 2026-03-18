# Planning Module + Memory Auto-Consolidation

**Date**: 2026-03-17
**Status**: Approved
**Modules**: Planning, Memory Consolidation

---

## 1. Planning Module

### 1.1 Goal

Add a Plan-then-Execute agent type that decomposes complex tasks into steps before execution. The current ReActAgent jumps straight into think-act loops without upfront planning, which leads to inefficient tool usage and inability to handle multi-step tasks.

### 1.2 Architecture

`PlanningAgent` extends `AgentBase<PlanningAgent>` and overrides `run()` with a three-phase loop:

1. **Plan** — LLM generates a structured plan from user input
2. **Execute** — Steps are executed sequentially; each step uses the existing think+act cycle
3. **Synthesize** — Results from all steps are combined into a final response

### 1.3 Data Structures

```cpp
// include/agentos/planning.hpp

enum class StepStatus { Pending, Running, Completed, Failed, Skipped };

struct PlanStep {
    std::string id;            // "step_1", "step_1.1"
    std::string description;   // What to do
    std::string tool_hint;     // Optional: suggested tool
    StepStatus status{StepStatus::Pending};
    std::string result;        // Execution result
    std::vector<PlanStep> substeps; // Dynamic decomposition
};

struct Plan {
    std::string goal;
    std::vector<PlanStep> steps;
    int revision{0};

    static constexpr int MAX_REVISIONS = 3;
    static constexpr int MAX_DEPTH = 3;
    static constexpr int MAX_STEPS = 10;
};
```

### 1.4 PlanningAgent

```cpp
class PlanningAgent : public AgentBase<PlanningAgent> {
public:
    using AgentBase<PlanningAgent>::AgentBase;
    Result<std::string> run(std::string user_input) override;

protected:
    Result<Plan> generate_plan(const std::string& goal);
    Result<std::string> execute_step(PlanStep& step, int depth = 0);
    Result<Plan> replan(Plan& current, const PlanStep& failed_step, const std::string& error);
    Result<std::string> synthesize(const Plan& plan);

private:
    bool needs_replanning(const PlanStep& step, const std::string& result);
    Plan parse_plan_from_llm(const std::string& llm_output, const std::string& goal);
};
```

### 1.5 run() Flow

```
run(user_input):
  // 1. Recall past similar plans from memory
  recall_results = recall(user_input, 3)
  inject recall context into system prompt

  // 2. Generate plan
  plan = generate_plan(user_input)

  // 3. Execute each step
  for step in plan.steps:
    step.status = Running
    result = execute_step(step)

    if step failed and plan.revision < MAX_REVISIONS:
      plan = replan(plan, step, error)
      continue from new step

  // 4. Synthesize
  final_answer = synthesize(plan)

  // 5. Remember the plan execution for future reference
  remember({goal, steps_summary, outcome}, importance=0.8)

  return final_answer
```

### 1.6 execute_step()

Each step runs a mini ReAct loop (max 3 iterations):
1. Send step description to LLM as user message
2. If LLM requests tool call → act, observe
3. If LLM gives text answer → step complete
4. If result indicates complexity → decompose into substeps (depth check)

### 1.7 Replan

Triggered when:
- Tool execution fails
- LLM response indicates a prerequisite is missing
- Step result contradicts the plan's assumptions

Replan does NOT regenerate the entire plan. It:
1. Marks the failed step as Failed
2. Sends the plan + failure context to LLM
3. LLM returns replacement steps for the remaining portion
4. Inserts new steps after the failed one

### 1.8 Plan Parsing

LLM is prompted to output plans in a structured format:
```
PLAN:
1. [step description] | tool: [optional tool hint]
2. [step description]
3. [step description]
```

Parser extracts steps via line-by-line parsing with regex. Falls back to treating entire output as a single step if parsing fails.

---

## 2. Memory Auto-Consolidation

### 2.1 Goal

Automate the flow of memories from short-term to long-term storage using an Ebbinghaus-inspired forgetting curve. Currently the three memory tiers (Working/Short/Long) have no automatic migration between them.

### 2.2 Memory Strength Model

```cpp
struct MemoryStrength {
    float initial_importance;   // Set at write time (0~1)
    float strength;             // Current strength (computed)
    TimePoint last_access;      // Last recall() hit
    uint32_t access_count{0};   // Number of recall() hits

    // Ebbinghaus: S(t) = S0 * e^(-lambda * t)
    // S0 boosted by recall: S0 = min(1.0, importance * (1 + 0.1 * access_count))
    float compute(TimePoint now) const;
};
```

Parameters:
- `decay_rate` (lambda): 0.3 per hour (configurable)
- `CONSOLIDATE_THRESHOLD`: 0.5 — migrate to long-term
- `FORGET_THRESHOLD`: 0.1 — delete from short-term

### 2.3 MemoryEntry Extension

Add to existing `MemoryEntry`:
```cpp
TimePoint last_access{now()};
uint32_t access_count{0};
```

`ShortTermMemory::search()` updates these fields on hit.

### 2.4 MemoryConsolidator

```cpp
// include/agentos/memory/consolidator.hpp

struct ConsolidatorConfig {
    Duration periodic_interval{120'000};     // 120s background scan
    size_t stm_count_threshold{100};         // Trigger on STM size
    float consolidate_threshold{0.5f};       // Migrate to LTM
    float forget_threshold{0.1f};            // Delete from STM
    float decay_rate{0.3f};                  // Lambda (per hour)
};

class MemoryConsolidator : private NonCopyable {
public:
    explicit MemoryConsolidator(MemorySystem& memory, ConsolidatorConfig cfg = {});
    ~MemoryConsolidator();  // Stops background thread

    void start();           // Launch background thread
    void stop();            // Stop background thread

    // Event-driven triggers
    void on_agent_run_complete(AgentId agent_id);
    void on_agent_destroyed(AgentId agent_id);

    // Manual trigger (for testing)
    ConsolidationResult consolidate_now(AgentId agent_id);

    struct ConsolidationResult {
        size_t scanned{0};
        size_t consolidated{0};  // Migrated to LTM
        size_t forgotten{0};     // Deleted from STM
        size_t retained{0};      // Kept in STM
    };

private:
    void background_loop(std::stop_token st);
    ConsolidationResult consolidate_agent(AgentId agent_id);

    MemorySystem& memory_;
    ConsolidatorConfig config_;
    std::jthread background_thread_;
    std::mutex mu_;
    std::unordered_set<AgentId> registered_agents_;
};
```

### 2.5 Consolidation Flow

```
consolidate_agent(agent_id):
  entries = memory_.short_term().get_all(agent_id)
  now = Clock::now()

  for entry in entries:
    strength = compute_strength(entry, now)

    if strength >= CONSOLIDATE_THRESHOLD:
      memory_.long_term().write(entry)
      memory_.short_term().forget(entry.id)
      result.consolidated++

    else if strength < FORGET_THRESHOLD:
      memory_.short_term().forget(entry.id)
      result.forgotten++

    else:
      result.retained++

  return result
```

### 2.6 Trigger Integration

| Event | Location | Action |
|-------|----------|--------|
| Agent run() returns | AgentBase::run_after_hooks or PlanningAgent::run() | `consolidator_->on_agent_run_complete(id)` |
| STM write exceeds threshold | ShortTermMemory::write() | Check count, trigger if needed |
| Background timer | MemoryConsolidator::background_loop | Scan all registered agents |
| Agent destroyed | AgentOS::destroy_agent() | `consolidator_->on_agent_destroyed(id)` |

---

## 3. Module Interaction

### 3.1 Planning x Memory

1. **Plan recall**: `generate_plan()` calls `recall()` to find similar past plans as LLM context
2. **Plan memorization**: Completed plans written to memory with importance=0.8
3. **Failure learning**: Replan events written to memory with importance=0.7 to avoid repeating mistakes

### 3.2 Consolidation x Planning

Planning memories (high importance, frequently recalled for similar tasks) naturally survive the forgetting curve and migrate to long-term storage. One-off plans with no future relevance decay and are forgotten.

---

## 4. File Structure

### New Files
- `include/agentos/planning.hpp` — Plan, PlanStep, PlanningAgent
- `include/agentos/memory/consolidator.hpp` — MemoryConsolidator declaration
- `src/memory/consolidator.cpp` — Consolidation logic
- `tests/test_planning.cpp` — Planning tests
- `tests/test_consolidator.cpp` — Consolidation tests

### Modified Files
- `include/agentos/memory/memory.hpp` — MemoryEntry: add last_access, access_count
- `src/memory/memory.cpp` — ShortTermMemory::search() updates access stats
- `include/agentos/agent.hpp` — AgentOS: add consolidator_ member
- `include/agentos/agentos.hpp` — Builder: consolidator config
- `CMakeLists.txt` — New compilation units and test files

---

## 5. Testing Strategy

### Planning Tests (test_planning.cpp)
- **Basic plan generation**: MockLLM returns structured plan, verify parsing
- **Step execution**: Mock tool calls in steps, verify sequential execution
- **Replan on failure**: Tool fails → verify replan triggers and inserts new steps
- **Depth limit**: Nested substeps respect MAX_DEPTH=3
- **Plan recall**: Verify past plans are recalled as context
- **MAX_REVISIONS**: Verify plan gives up after 3 replans

### Consolidation Tests (test_consolidator.cpp)
- **Strength calculation**: Verify decay math with known time deltas
- **Consolidation thresholds**: High-strength → LTM, low-strength → forgotten
- **Access strengthening**: Entries recalled multiple times survive longer
- **Event triggers**: Run complete, agent destroyed, threshold exceeded
- **Background thread**: Start/stop, periodic execution
- **Agent isolation**: Consolidation of one agent doesn't affect another

### Integration Tests
- PlanningAgent run → memory written → consolidator migrates to LTM → future recall finds it
