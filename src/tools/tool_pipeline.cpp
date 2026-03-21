#include <agentos/tools/tool_pipeline.hpp>
#include <future>

namespace agentos::tools {

ToolPipeline& ToolPipeline::then(std::string tool_name,
                                  std::function<std::string(const std::string&)> transform) {
    PipelineStep step;
    step.tool_name = std::move(tool_name);
    step.transform = std::move(transform);
    steps_.push_back(std::move(step));
    return *this;
}

ToolPipeline& ToolPipeline::then_if(std::function<bool(const std::string&)> condition,
                                     std::string tool_name,
                                     std::function<std::string(const std::string&)> transform) {
    PipelineStep step;
    step.tool_name = std::move(tool_name);
    step.transform = std::move(transform);
    step.condition = std::move(condition);
    steps_.push_back(std::move(step));
    return *this;
}

PipelineResult ToolPipeline::execute(const std::string& initial_input) {
    PipelineResult result;
    std::string current_output = initial_input;

    for (const auto& step : steps_) {
        // Check condition
        if (step.condition && !step.condition(current_output)) {
            continue;  // skip this step
        }

        // Transform input to args
        std::string args_json;
        if (step.transform) {
            args_json = step.transform(current_output);
        } else {
            // Default: pass as {"input": "..."}
            nlohmann::json j;
            j["input"] = current_output;
            args_json = j.dump();
        }

        // Execute tool
        kernel::ToolCallRequest call;
        call.id = "pipeline-step-" + std::to_string(result.steps_executed);
        call.name = step.tool_name;
        call.args_json = args_json;

        auto step_result = manager_.dispatch(call);
        result.step_results.push_back(step_result);
        result.steps_executed++;

        if (!step_result.success) {
            result.error = "Step " + std::to_string(result.steps_executed) +
                          " (" + step.tool_name + ") failed: " + step_result.error;
            return result;
        }

        current_output = step_result.output;
    }

    result.success = true;
    result.final_output = current_output;
    return result;
}

// ── Parallel execution ──

ToolParallel& ToolParallel::add(std::string tool_name, std::string args_json) {
    calls_.emplace_back(std::move(tool_name), std::move(args_json));
    return *this;
}

ParallelResult ToolParallel::execute() {
    ParallelResult result;
    result.results.resize(calls_.size());

    std::vector<std::future<ToolResult>> futures;
    futures.reserve(calls_.size());

    for (size_t i = 0; i < calls_.size(); ++i) {
        const auto& name = calls_[i].first;
        const auto& args = calls_[i].second;
        futures.push_back(std::async(std::launch::async, [this, &name, &args, i]() {
            kernel::ToolCallRequest call;
            call.id = "parallel-" + std::to_string(i);
            call.name = name;
            call.args_json = args;
            return manager_.dispatch(call);
        }));
    }

    result.all_success = true;
    for (size_t i = 0; i < futures.size(); ++i) {
        result.results[i] = futures[i].get();
        if (!result.results[i].success) {
            result.all_success = false;
        }
    }

    return result;
}

} // namespace agentos::tools
