// ============================================================
// Tests :: Circuit Breaker
// ============================================================
#include <agentos/core/circuit_breaker.hpp>
#include <gtest/gtest.h>
#include <thread>

using namespace agentos;

TEST(CircuitBreaker, ClosedAllowsRequests) {
    CircuitBreaker cb("test");
    EXPECT_TRUE(cb.allow_request());
    EXPECT_TRUE(cb.allow_request());
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::Closed);
}

TEST(CircuitBreaker, TripsAfterThreshold) {
    CircuitBreakerConfig cfg{.failure_threshold = 3, .recovery_time = std::chrono::seconds(30)};
    CircuitBreaker cb("test", cfg);

    // First two failures: still closed
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allow_request());

    // Third failure: trips to open
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);
    EXPECT_FALSE(cb.allow_request());
    EXPECT_FALSE(cb.allow_request());
}

TEST(CircuitBreaker, RecoveryAfterTimeout) {
    CircuitBreakerConfig cfg{
        .failure_threshold = 3,
        .recovery_time = std::chrono::seconds(1),
        .half_open_max_calls = 2
    };
    CircuitBreaker cb("test", cfg);

    // Trip the breaker
    for (size_t i = 0; i < 3; ++i) cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);
    EXPECT_FALSE(cb.allow_request());

    // Wait for recovery
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // Should transition to half-open and allow a probe
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::HalfOpen);
}

TEST(CircuitBreaker, HalfOpenSuccessCloses) {
    CircuitBreakerConfig cfg{
        .failure_threshold = 3,
        .recovery_time = std::chrono::seconds(1),
        .half_open_max_calls = 2
    };
    CircuitBreaker cb("test", cfg);

    // Trip and wait for recovery
    for (size_t i = 0; i < 3; ++i) cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cb.allow_request());  // transitions to half-open

    // Success in half-open should close
    cb.record_success();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allow_request());
}

TEST(CircuitBreaker, HalfOpenFailureReopens) {
    CircuitBreakerConfig cfg{
        .failure_threshold = 3,
        .recovery_time = std::chrono::seconds(1),
        .half_open_max_calls = 2
    };
    CircuitBreaker cb("test", cfg);

    // Trip and wait for recovery
    for (size_t i = 0; i < 3; ++i) cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    EXPECT_TRUE(cb.allow_request());  // transitions to half-open

    // Failure in half-open should re-open
    cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);
    EXPECT_FALSE(cb.allow_request());
}

TEST(CircuitBreaker, ManualReset) {
    CircuitBreakerConfig cfg{.failure_threshold = 3};
    CircuitBreaker cb("test", cfg);

    // Trip the breaker
    for (size_t i = 0; i < 3; ++i) cb.record_failure();
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // Manual reset
    cb.reset();
    EXPECT_EQ(cb.state(), CircuitState::Closed);
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.failure_count(), 0u);
}

TEST(CircuitBreaker, Metrics) {
    CircuitBreakerConfig cfg{
        .failure_threshold = 3,
        .recovery_time = std::chrono::seconds(30)
    };
    CircuitBreaker cb("test", cfg);

    EXPECT_EQ(cb.failure_count(), 0u);
    EXPECT_EQ(cb.success_count(), 0u);
    EXPECT_EQ(cb.total_rejected(), 0u);

    cb.record_success();
    cb.record_success();
    EXPECT_EQ(cb.success_count(), 2u);

    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    EXPECT_EQ(cb.failure_count(), 3u);
    EXPECT_EQ(cb.state(), CircuitState::Open);

    // Rejected calls
    EXPECT_FALSE(cb.allow_request());
    EXPECT_FALSE(cb.allow_request());
    EXPECT_EQ(cb.total_rejected(), 2u);

    EXPECT_EQ(cb.state_string(), "Open");
    EXPECT_EQ(cb.name(), "test");
}

TEST(CircuitBreaker, HalfOpenMaxCalls) {
    CircuitBreakerConfig cfg{
        .failure_threshold = 2,
        .recovery_time = std::chrono::seconds(1),
        .half_open_max_calls = 2
    };
    CircuitBreaker cb("test", cfg);

    // Trip and wait
    cb.record_failure();
    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));

    // First call transitions to half-open and is allowed (counts as 1 of 2)
    EXPECT_TRUE(cb.allow_request());
    EXPECT_EQ(cb.state(), CircuitState::HalfOpen);

    // Second call in half-open is allowed (2 of 2)
    EXPECT_TRUE(cb.allow_request());

    // Third call exceeds half_open_max_calls
    EXPECT_FALSE(cb.allow_request());
}
