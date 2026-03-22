// Tool Composition Pipeline
#include <agentos/agentos.hpp>
#include <agentos/tools/tool_pipeline.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Sequential pipeline: set then get
    tools::ToolPipeline pipe(os->tools());
    pipe.then("kv_store", [](const std::string&) {
        return R"({"op":"set","key":"greeting","value":"Hello, Pipeline!"})";
    }).then("kv_store", [](const std::string&) {
        return R"({"op":"get","key":"greeting"})";
    });

    auto result = pipe.execute("");
    std::cout << "Pipeline result: " << result.final_output << "\n";
    std::cout << "Steps executed: " << result.steps_executed << "\n";

    // Parallel execution
    tools::ToolParallel par(os->tools());
    par.add("kv_store", R"({"op":"set","key":"a","value":"1"})");
    par.add("kv_store", R"({"op":"set","key":"b","value":"2"})");
    par.add("kv_store", R"({"op":"set","key":"c","value":"3"})");

    auto par_result = par.execute();
    std::cout << "\nParallel: " << par_result.results.size() << " tools executed"
              << (par_result.all_success ? " (all OK)" : " (some failed)") << "\n";

    return 0;
}
