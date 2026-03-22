// Multi-Backend Demo: Ollama, Anthropic, and Router
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

int main() {
    // --- Option 1: Ollama (local) ---
    // auto os = AgentOSBuilder().ollama("llama3").build();

    // --- Option 2: Anthropic Claude ---
    // auto os = AgentOSBuilder().anthropic("sk-ant-...").build();

    // --- Option 3: Router (route by complexity) ---
    // auto router = std::make_unique<kernel::RouterBackend>();
    // auto simple = std::make_shared<kernel::OllamaBackend>("llama3");
    // auto powerful = std::make_shared<kernel::AnthropicBackend>("sk-ant-...");
    // router->set_default(simple);
    // router->add_rule({
    //     .name = "complex-tasks",
    //     .predicate = kernel::routing::by_complexity(500),
    //     .backend = powerful
    // });
    // auto os = AgentOSBuilder().router(std::move(router)).build();

    // For this demo, use mock backend
    auto os = AgentOSBuilder().mock().security(false).build();

    auto agent = make_agent(*os, "demo")
        .prompt("You are a helpful assistant.")
        .create();

    auto reply = agent->run("Hello!");
    if (reply.has_value()) {
        std::cout << "Reply: " << *reply << "\n";
    }

    // Show metrics
    std::cout << "\n--- Prometheus Metrics ---\n";
    std::cout << os->metrics_prometheus() << "\n";

    return 0;
}
