#pragma once
// ============================================================
// AgentOS :: Core :: Circuit Breaker
// Production reliability — prevents cascading failures
// ============================================================
#include <agentos/core/types.hpp>
#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

namespace agentos {

enum class CircuitState { Closed, Open, HalfOpen };

struct CircuitBreakerConfig {
    size_t failure_threshold{5};              // failures before opening
    std::chrono::seconds recovery_time{30};   // time before trying half-open
    size_t half_open_max_calls{2};            // calls allowed in half-open
};

class CircuitBreaker : private NonCopyable {
public:
    explicit CircuitBreaker(std::string name, CircuitBreakerConfig config = {});

    // Check if request should be allowed
    bool allow_request();

    // Record success (resets failure count, may close from half-open)
    void record_success();

    // Record failure (increments count, may trip to open)
    void record_failure();

    // Current state
    CircuitState state() const;
    std::string state_string() const;

    // Metrics
    size_t failure_count() const;
    size_t success_count() const;
    size_t total_rejected() const;

    // Manual reset
    void reset();

    const std::string& name() const { return name_; }

private:
    std::string name_;
    CircuitBreakerConfig config_;
    mutable std::mutex mu_;

    CircuitState state_{CircuitState::Closed};
    size_t failures_{0};
    size_t successes_{0};
    size_t half_open_calls_{0};
    size_t rejected_{0};
    std::chrono::steady_clock::time_point opened_at_;
};

} // namespace agentos
