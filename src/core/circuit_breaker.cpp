// ============================================================
// AgentOS :: Core :: Circuit Breaker — Implementation
// ============================================================
#include <agentos/core/circuit_breaker.hpp>

namespace agentos {

CircuitBreaker::CircuitBreaker(std::string name, CircuitBreakerConfig config)
    : name_(std::move(name)), config_(config) {}

bool CircuitBreaker::allow_request() {
    std::lock_guard lk(mu_);
    switch (state_) {
        case CircuitState::Closed:
            return true;
        case CircuitState::Open: {
            auto elapsed = std::chrono::steady_clock::now() - opened_at_;
            if (elapsed >= config_.recovery_time) {
                state_ = CircuitState::HalfOpen;
                half_open_calls_ = 1;  // this call counts
                return true;
            }
            rejected_++;
            return false;
        }
        case CircuitState::HalfOpen:
            if (half_open_calls_ < config_.half_open_max_calls) {
                half_open_calls_++;
                return true;
            }
            rejected_++;
            return false;
    }
    return false;
}

void CircuitBreaker::record_success() {
    std::lock_guard lk(mu_);
    successes_++;
    if (state_ == CircuitState::HalfOpen) {
        state_ = CircuitState::Closed;
        failures_ = 0;
    }
}

void CircuitBreaker::record_failure() {
    std::lock_guard lk(mu_);
    failures_++;
    if (state_ == CircuitState::Closed && failures_ >= config_.failure_threshold) {
        state_ = CircuitState::Open;
        opened_at_ = std::chrono::steady_clock::now();
    } else if (state_ == CircuitState::HalfOpen) {
        state_ = CircuitState::Open;
        opened_at_ = std::chrono::steady_clock::now();
    }
}

CircuitState CircuitBreaker::state() const {
    std::lock_guard lk(mu_);
    return state_;
}

std::string CircuitBreaker::state_string() const {
    switch (state()) {
        case CircuitState::Closed:   return "Closed";
        case CircuitState::Open:     return "Open";
        case CircuitState::HalfOpen: return "HalfOpen";
    }
    return "Unknown";
}

size_t CircuitBreaker::failure_count() const {
    std::lock_guard lk(mu_);
    return failures_;
}

size_t CircuitBreaker::success_count() const {
    std::lock_guard lk(mu_);
    return successes_;
}

size_t CircuitBreaker::total_rejected() const {
    std::lock_guard lk(mu_);
    return rejected_;
}

void CircuitBreaker::reset() {
    std::lock_guard lk(mu_);
    state_ = CircuitState::Closed;
    failures_ = 0;
    half_open_calls_ = 0;
    // Note: don't reset successes_ or rejected_ — those are cumulative metrics
}

} // namespace agentos
