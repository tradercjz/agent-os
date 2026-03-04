#pragma once
// ============================================================
// AgentOS :: Agent Bus — Hub-and-Spoke 通信总线
// 支持 Request/Response + Pub/Sub，Hub 负责安全过滤
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/security/security.hpp>
#include <any>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

namespace agentos::bus {

// ─────────────────────────────────────────────────────────────
// § B.1  Message — 消息类型
// ─────────────────────────────────────────────────────────────

enum class MessageType {
    Request,    // 发送方等待响应
    Response,   // 响应消息
    Event,      // 广播事件（Pub/Sub）
    Heartbeat,  // 心跳（存活检测）
};

struct BusMessage {
    uint64_t            id;
    MessageType         type;
    AgentId             from;
    AgentId             to;        // 0 = 广播
    std::string         topic;     // Pub/Sub 主题 / RPC 方法名
    std::string         payload;   // JSON 字符串
    uint64_t            reply_to{0};  // 对哪个 Request 的响应
    TimePoint           timestamp{now()};
    bool                redacted{false}; // Hub 脱敏标记

    static BusMessage make_request(AgentId from, AgentId to,
                                   std::string topic, std::string payload) {
        static std::atomic<uint64_t> id_gen{1};
        return {id_gen++, MessageType::Request, from, to,
                std::move(topic), std::move(payload)};
    }

    static BusMessage make_response(const BusMessage& req, std::string payload) {
        static std::atomic<uint64_t> id_gen{100000};
        return {id_gen++, MessageType::Response, req.to, req.from,
                req.topic, std::move(payload), req.id};
    }

    static BusMessage make_event(AgentId from, std::string topic,
                                  std::string payload) {
        static std::atomic<uint64_t> id_gen{200000};
        return {id_gen++, MessageType::Event, from, 0,
                std::move(topic), std::move(payload)};
    }
};

// ─────────────────────────────────────────────────────────────
// § B.2  Channel — Agent 的消息队列（Spoke）
// ─────────────────────────────────────────────────────────────

class Channel {
public:
    explicit Channel(AgentId owner) : owner_(owner) {}

    void push(BusMessage msg) {
        std::lock_guard lk(mu_);
        queue_.push(std::move(msg));
        cv_.notify_one();
    }

    // 阻塞等待消息
    std::optional<BusMessage> recv(Duration timeout = Duration{5000}) {
        std::unique_lock lk(mu_);
        if (!cv_.wait_for(lk, timeout, [this] { return !queue_.empty(); }))
            return std::nullopt;
        auto msg = queue_.front();
        queue_.pop();
        return msg;
    }

    // 非阻塞接收
    std::optional<BusMessage> try_recv() {
        std::lock_guard lk(mu_);
        if (queue_.empty()) return std::nullopt;
        auto msg = queue_.front();
        queue_.pop();
        return msg;
    }

    bool empty() const {
        std::lock_guard lk(mu_);
        return queue_.empty();
    }

    AgentId owner() const { return owner_; }

private:
    AgentId owner_;
    std::queue<BusMessage> queue_;
    mutable std::mutex mu_;
    std::condition_variable cv_;
};

// ─────────────────────────────────────────────────────────────
// § B.3  AgentBus — Hub（中央路由 + 安全过滤）
// ─────────────────────────────────────────────────────────────

using MessageHandler = std::function<void(const BusMessage&)>;

class AgentBus : private NonCopyable {
public:
    explicit AgentBus(security::SecurityManager* sec = nullptr)
        : security_(sec) {}

    // ── Spoke 注册/注销 ─────────────────────────────────────
    std::shared_ptr<Channel> register_agent(AgentId id) {
        std::lock_guard lk(mu_);
        auto ch = std::make_shared<Channel>(id);
        channels_[id] = ch;
        return ch;
    }

    void unregister_agent(AgentId id) {
        std::lock_guard lk(mu_);
        channels_.erase(id);
        subscriptions_.erase(id);
    }

    // ── Pub/Sub ─────────────────────────────────────────────
    void subscribe(AgentId id, std::string topic) {
        std::lock_guard lk(mu_);
        subscriptions_[id].insert(std::move(topic));
    }

    void unsubscribe(AgentId id, const std::string& topic) {
        std::lock_guard lk(mu_);
        if (auto it = subscriptions_.find(id); it != subscriptions_.end())
            it->second.erase(topic);
    }

    void publish(BusMessage event) {
        if (event.type != MessageType::Event) return;

        // 安全扫描
        if (security_) {
            auto det = security_->ecl().scan_llm_output(event.from,
                                                         kernel::LLMResponse{});
            // 内容扫描（简化，仅记录）
        }

        std::lock_guard lk(mu_);
        audit_push(event);

        for (auto& [agent_id, topics] : subscriptions_) {
            if (topics.count(event.topic)) {
                auto it = channels_.find(agent_id);
                if (it != channels_.end()) {
                    auto redacted_event = redact_if_needed(event, agent_id);
                    it->second->push(redacted_event);
                }
            }
        }
    }

    // ── Request/Response ────────────────────────────────────
    void send(BusMessage msg) {
        // Hub 做安全过滤：Spoke 间不允许直接通信，必须经 Hub
        if (security_) {
            // 扫描 payload 中的注入尝试
            auto det_result = security_->detector().scan(msg.payload);
            if (det_result.is_injection) {
                // 脱敏：标记并截断可疑内容
                msg.payload = "[REDACTED: injection detected]";
                msg.redacted = true;
            }
        }

        audit_push(msg);

        std::lock_guard lk(mu_);
        if (msg.to == 0) {
            // 广播
            for (auto& [id, ch] : channels_) {
                if (id != msg.from) ch->push(msg);
            }
        } else {
            auto it = channels_.find(msg.to);
            if (it != channels_.end()) {
                it->second->push(msg);
            }
        }
    }

    // 同步 RPC：发送 Request 并等待 Response
    std::optional<BusMessage> call(AgentId caller,
                                   AgentId target,
                                   std::string method,
                                   std::string payload,
                                   Duration timeout = Duration{10000}) {
        // 找到调用方的 Channel
        std::shared_ptr<Channel> caller_ch;
        {
            std::lock_guard lk(mu_);
            auto it = channels_.find(caller);
            if (it == channels_.end()) return std::nullopt;
            caller_ch = it->second;
        }

        auto req = BusMessage::make_request(caller, target,
                                            std::move(method), std::move(payload));
        uint64_t req_id = req.id;
        send(std::move(req));

        // 等待对应的 Response
        auto deadline = now() + timeout;
        while (now() < deadline) {
            auto msg = caller_ch->recv(Duration{100});
            if (msg && msg->type == MessageType::Response &&
                msg->reply_to == req_id) {
                return msg;
            }
        }
        return std::nullopt;
    }

    // ── 观察者模式（系统级监控）─────────────────────────────
    void add_monitor(MessageHandler h) {
        std::lock_guard lk(mu_);
        monitors_.push_back(std::move(h));
    }

    const std::vector<BusMessage>& audit_trail() const { return audit_trail_; }

private:
    BusMessage redact_if_needed(BusMessage msg, AgentId /*recipient*/) const {
        // 可以在这里基于接收方权限脱敏敏感字段
        return msg;
    }

    void audit_push(const BusMessage& msg) {
        audit_trail_.push_back(msg);
        for (auto& m : monitors_) m(msg);
        // 限制审计日志大小
        if (audit_trail_.size() > 10000) {
            audit_trail_.erase(audit_trail_.begin(),
                               audit_trail_.begin() + 1000);
        }
    }

    mutable std::mutex mu_;
    std::unordered_map<AgentId, std::shared_ptr<Channel>> channels_;
    std::unordered_map<AgentId, std::unordered_set<std::string>> subscriptions_;
    std::vector<BusMessage> audit_trail_;
    std::vector<MessageHandler> monitors_;
    security::SecurityManager* security_; // 非拥有指针
};

} // namespace agentos::bus
