#pragma once
// ============================================================
// AgentOS :: Headless Runner
// 无头执行器：JSON 配置 → 创建 Agent → 执行任务 → 返回结果
// 用于 CI/CD、webhook、GitHub Actions 集成
// ============================================================
#include <agentos/agent.hpp>
#include <chrono>
#include <thread>
#include <string>

namespace agentos::headless {

// ── Request / Result types ──

struct RunRequest {
    std::string task;                              // user input / task description
    std::string agent_name{"headless-agent"};
    std::string role_prompt{"You are a helpful assistant."};
    Duration timeout{Duration{60000}};             // default 60s
    TokenCount context_limit{8192};
    std::vector<std::string> allowed_tools{};

    static RunRequest from_json(const Json& j) {
        RunRequest req;
        if (j.contains("task")) req.task = j["task"].get<std::string>();
        if (j.contains("agent_name")) req.agent_name = j["agent_name"].get<std::string>();
        if (j.contains("role_prompt")) req.role_prompt = j["role_prompt"].get<std::string>();
        if (j.contains("timeout_ms")) req.timeout = Duration{j["timeout_ms"].get<int64_t>()};
        if (j.contains("context_limit")) req.context_limit = j["context_limit"].get<TokenCount>();
        if (j.contains("allowed_tools")) {
            for (const auto& t : j["allowed_tools"]) {
                req.allowed_tools.push_back(t.get<std::string>());
            }
        }
        return req;
    }

    Json to_json() const {
        Json j;
        j["task"] = task;
        j["agent_name"] = agent_name;
        j["role_prompt"] = role_prompt;
        j["timeout_ms"] = timeout.count();
        j["context_limit"] = context_limit;
        j["allowed_tools"] = allowed_tools;
        return j;
    }
};

struct RunResult {
    bool success{false};
    std::string output;
    std::string error;
    uint64_t duration_ms{0};
    uint64_t tokens_used{0};

    Json to_json() const {
        Json j;
        j["success"] = success;
        j["output"] = output;
        j["error"] = error;
        j["duration_ms"] = duration_ms;
        j["tokens_used"] = tokens_used;
        return j;
    }
};

// ── Headless Runner ──

class HeadlessRunner {
public:
    explicit HeadlessRunner(std::unique_ptr<kernel::ILLMBackend> backend,
                            AgentOS::Config os_config = AgentOS::Config::builder().build())
        : os_(std::make_shared<AgentOS>(std::move(backend), std::move(os_config))) {}

    /// Run a task and return structured result
    RunResult run(const RunRequest& req) {
        if (req.task.empty()) {
            return {.success = false, .output = "", .error = "Empty task",
                    .duration_ms = 0, .tokens_used = 0};
        }

        auto start = Clock::now();
        auto os = os_;
        uint64_t tokens_before = os->kernel().metrics().total_tokens.load();

        // Create agent
        auto agent = os->create_agent(
            AgentConfig::builder()
                .name(req.agent_name)
                .role_prompt(req.role_prompt)
                .context_limit(req.context_limit)
                .tools(req.allowed_tools)
                .build());
        AgentId aid = agent->id();

        // Run on a detached worker so timeout returns promptly without
        // destroying the running agent from this call frame.
        auto promise = std::make_shared<std::promise<Result<std::string>>>();
        auto future = promise->get_future();
        std::thread([os, agent, promise, task = req.task, aid]() mutable {
            try {
                promise->set_value(agent->run(std::move(task)));
            } catch (...) {
                promise->set_exception(std::current_exception());
            }
            os->destroy_agent(aid);
        }).detach();

        auto status = future.wait_for(
            std::chrono::milliseconds(req.timeout.count()));

        auto elapsed = std::chrono::duration_cast<Duration>(Clock::now() - start);
        uint64_t tokens_after = os->kernel().metrics().total_tokens.load();

        if (status == std::future_status::timeout) {
            return {.success = false, .output = "",
                    .error = "Task timed out",
                    .duration_ms = static_cast<uint64_t>(elapsed.count()),
                    .tokens_used = tokens_after - tokens_before};
        }

        auto result = future.get();

        RunResult run_result;
        run_result.duration_ms = static_cast<uint64_t>(elapsed.count());
        run_result.tokens_used = tokens_after - tokens_before;

        if (result) {
            run_result.success = true;
            run_result.output = result.value();
        } else {
            run_result.success = false;
            run_result.error = result.error().message;
        }

        return run_result;
    }

    /// Run from JSON string
    RunResult run_json(const std::string& json_str) {
        Json j;
        try {
            j = Json::parse(json_str);
        } catch (...) {
            return {.success = false, .output = "",
                    .error = "Invalid JSON", .duration_ms = 0, .tokens_used = 0};
        }
        return run(RunRequest::from_json(j));
    }

    /// Access the underlying AgentOS (for tool registration etc.)
    AgentOS& os() { return *os_; }

private:
    std::shared_ptr<AgentOS> os_;
};

} // namespace agentos::headless
