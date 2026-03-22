// Observability: Tracing, Metrics, Circuit Breaker, Lifecycle
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Lifecycle events
    os->lifecycle().on(LifecycleEvent::AgentCreated,
        [](const LifecycleEventData& e) {
            std::cout << "[Event] Agent created: " << e.agent_name << "\n";
        });

    auto agent = make_agent(*os, "traced-bot").prompt("You are traced.").create();

    // Tracing with ScopedSpan
    auto& tracer = os->tracer();
    auto tid = tracer.begin_trace(agent->id(), "demo-trace");
    {
        tracing::ScopedSpan span(tracer, tid, "", "inference");
        (void)agent->run("Hello");
        span.set_tokens(100);
    }
    tracer.end_trace(tid);

    // Export as OTLP JSON
    auto otlp = tracer.export_otlp_json(tid);
    std::cout << "\nOTLP trace (first 200 chars):\n"
              << otlp.substr(0, 200) << "...\n";

    // Prometheus metrics
    std::cout << "\n--- Prometheus ---\n" << os->metrics_prometheus();

    // Circuit breaker
    auto& cb = os->circuit_breaker();
    std::cout << "\nCircuit breaker state: " << cb.state_string() << "\n";

    // Per-agent rate limiting
    os->agent_rate_limiter().set_quota(agent->id(), {.tpm = 50000});
    std::cout << "Agent " << agent->id() << " TPM quota: 50000\n";

    return 0;
}
