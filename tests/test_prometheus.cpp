#include <agentos/agentos.hpp>
#include <gtest/gtest.h>

using namespace agentos;

TEST(PrometheusFormatterTest, FormatContainsKernelMetrics) {
    auto os = quickstart_mock();
    auto output = os->metrics_prometheus();

    EXPECT_NE(output.find("agentos_kernel_requests_total"), std::string::npos);
    EXPECT_NE(output.find("agentos_kernel_tokens_total"), std::string::npos);
    EXPECT_NE(output.find("agentos_kernel_errors_total"), std::string::npos);
    EXPECT_NE(output.find("# TYPE agentos_kernel_requests_total counter"), std::string::npos);
}

TEST(PrometheusFormatterTest, FormatContainsSchedulerMetrics) {
    auto os = quickstart_mock();
    auto output = os->metrics_prometheus();

    EXPECT_NE(output.find("agentos_scheduler_tasks_completed_total"), std::string::npos);
    EXPECT_NE(output.find("agentos_scheduler_active_tasks"), std::string::npos);
    EXPECT_NE(output.find("# TYPE agentos_scheduler_active_tasks gauge"), std::string::npos);
}

TEST(PrometheusFormatterTest, FormatContainsMemoryMetrics) {
    auto os = quickstart_mock();
    auto output = os->metrics_prometheus();

    EXPECT_NE(output.find("agentos_memory_entries{tier=\"working\"}"), std::string::npos);
    EXPECT_NE(output.find("agentos_memory_entries{tier=\"short_term\"}"), std::string::npos);
    EXPECT_NE(output.find("agentos_memory_entries{tier=\"long_term\"}"), std::string::npos);
}

TEST(PrometheusFormatterTest, FormatContainsBusMetrics) {
    auto os = quickstart_mock();
    auto output = os->metrics_prometheus();

    EXPECT_NE(output.find("agentos_bus_dropped_total"), std::string::npos);
}

TEST(PrometheusFormatterTest, MetricsReflectChanges) {
    auto os = quickstart_mock();

    // Do some inference to bump metrics
    kernel::LLMRequest req;
    req.messages.push_back(kernel::Message::user("hello"));
    (void)os->kernel().infer(req);

    auto output = os->metrics_prometheus();
    // Should show at least 1 request
    EXPECT_NE(output.find("agentos_kernel_requests_total 1"), std::string::npos);
}
