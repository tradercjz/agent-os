#pragma once
// ============================================================
// AgentOS :: Tool Pipeline — Sequential & Parallel Composition
// ============================================================
#include <agentos/tools/tool_manager.hpp>
#include <functional>
#include <future>
#include <string>
#include <vector>

namespace agentos::tools {

// A step in a tool pipeline
struct PipelineStep {
    std::string tool_name;
    // Transform the output of the previous step into args for this step
    // Input: previous step's output string
    // Output: JSON args string for this tool
    std::function<std::string(const std::string&)> transform;

    // Optional: condition to decide if this step should run
    std::function<bool(const std::string&)> condition;
};

// Result of a pipeline execution
struct PipelineResult {
    bool success{false};
    std::string final_output;
    std::vector<ToolResult> step_results;  // result of each step
    size_t steps_executed{0};
    std::string error;
};

// Compose multiple tools into a sequential pipeline
class ToolPipeline {
public:
    explicit ToolPipeline(ToolManager& manager) : manager_(manager) {}

    // Add a step to the pipeline
    ToolPipeline& then(std::string tool_name,
                       std::function<std::string(const std::string&)> transform = nullptr);

    // Add a conditional step
    ToolPipeline& then_if(std::function<bool(const std::string&)> condition,
                          std::string tool_name,
                          std::function<std::string(const std::string&)> transform = nullptr);

    // Execute the pipeline with initial input
    PipelineResult execute(const std::string& initial_input);

    // Get step count
    size_t step_count() const { return steps_.size(); }

    // Clear all steps
    void clear() { steps_.clear(); }

private:
    ToolManager& manager_;
    std::vector<PipelineStep> steps_;
};

// Parallel execution: run multiple tools concurrently, collect results
struct ParallelResult {
    std::vector<ToolResult> results;
    bool all_success{false};
};

class ToolParallel {
public:
    explicit ToolParallel(ToolManager& manager) : manager_(manager) {}

    // Add a tool to run in parallel
    ToolParallel& add(std::string tool_name, std::string args_json);

    // Execute all tools concurrently
    ParallelResult execute();

    size_t tool_count() const { return calls_.size(); }

private:
    ToolManager& manager_;
    std::vector<std::pair<std::string, std::string>> calls_;  // name, args_json
};

} // namespace agentos::tools
