// Agent Reasoning Patterns: CoT and Plan-Execute
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Chain-of-Thought agent
    std::cout << "=== Chain-of-Thought Agent ===\n";
    auto cot = make_agent(*os, "thinker")
        .prompt("You are a math tutor. Show your reasoning.")
        .create_cot();

    auto cot_result = cot->run("What is 15% of 240?");
    if (cot_result.has_value()) {
        std::cout << "CoT Result: " << *cot_result << "\n\n";
    }

    // Plan-and-Execute agent
    std::cout << "=== Plan-Execute Agent ===\n";
    auto planner = make_agent(*os, "planner")
        .prompt("You are a project manager.")
        .create_plan_execute();

    auto plan_result = planner->run("Organize a team meeting for next week");
    if (plan_result.has_value()) {
        std::cout << "Plan Result: " << *plan_result << "\n";
    }

    std::cout << "\nPlan steps:\n";
    for (const auto& step : planner->current_plan()) {
        std::cout << "  Step " << step.step_number << " [" << step.status << "]: "
                  << step.description << "\n";
    }

    return 0;
}
