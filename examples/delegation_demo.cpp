// Multi-Agent Delegation and Shared Memory
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Create agents
    auto coordinator = make_agent(*os, "coordinator")
        .prompt("You coordinate tasks between team members.")
        .create();
    auto researcher = make_agent(*os, "researcher")
        .prompt("You research topics and provide summaries.")
        .create();
    auto writer = make_agent(*os, "writer")
        .prompt("You write polished content from research notes.")
        .create();

    // Set up teams
    (void)os->teams().create_team("content-team");
    (void)os->teams().assign(coordinator->id(), "content-team");
    (void)os->teams().assign(researcher->id(), "content-team");
    (void)os->teams().assign(writer->id(), "content-team");

    // Delegate research task
    DelegationRequest req;
    req.from = coordinator->id();
    req.to = researcher->id();
    req.task = "Research the latest trends in AI agents";
    auto result = os->delegation().delegate(req);

    if (result.has_value()) {
        // Store result in shared memory
        os->shared_memory().put("research_notes", result->output, coordinator->id());
        std::cout << "Research result stored in shared memory\n";

        // Writer can access shared memory
        auto notes = os->shared_memory().get("research_notes");
        if (notes) {
            std::cout << "Writer sees: " << notes->substr(0, 100) << "...\n";
        }
    }

    // Fan out to multiple agents
    auto results = os->delegation().fan_out(
        coordinator->id(),
        {researcher->id(), writer->id()},
        "Summarize your capabilities");

    std::cout << "\nFan-out results: " << results.size() << " responses\n";

    return 0;
}
