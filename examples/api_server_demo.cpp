// REST API Server Demo
#include <agentos/agentos.hpp>
#include <iostream>

using namespace agentos;

int main() {
    auto os = AgentOSBuilder().mock().security(false).build();

    // Start REST API on port 9090
    auto r = os->start_api(9090);
    if (!r.has_value()) {
        std::cerr << "Failed to start API: " << r.error().message << "\n";
        return 1;
    }

    // Start Dashboard on port 8080
    (void)os->start_dashboard(8080);

    std::cout << "REST API running at http://localhost:9090\n";
    std::cout << "Dashboard running at http://localhost:8080\n";
    std::cout << "\nEndpoints:\n";
    std::cout << "  GET  /api/v1/health       - Health check\n";
    std::cout << "  GET  /api/v1/agents       - List agents\n";
    std::cout << "  POST /api/v1/agents       - Create agent\n";
    std::cout << "  POST /api/v1/infer        - Run inference\n";
    std::cout << "  GET  /api/v1/metrics      - Metrics JSON\n";
    std::cout << "  GET  /api/v1/prometheus   - Prometheus format\n";
    std::cout << "\nExample:\n";
    std::cout << R"(  curl -X POST http://localhost:9090/api/v1/infer \)" << "\n";
    std::cout << R"(    -d '{"messages":[{"role":"user","content":"Hello"}]}')" << "\n";
    std::cout << "\nPress Enter to stop...\n";

    std::cin.get();
    return 0;
}
