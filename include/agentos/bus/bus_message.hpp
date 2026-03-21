#pragma once
// ============================================================
// AgentOS :: BusMessage — Message types for the agent bus
// Extracted to break circular dependency between agent_bus and audit_store
// ============================================================
#include <agentos/core/types.hpp>
#include <atomic>
#include <string>

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

    // Unified ID generator — avoids collisions between request/response/event IDs
    static uint64_t next_id() {
        static std::atomic<uint64_t> id_gen{1};
        // Relaxed ordering sufficient for monotonic ID counter (no data published via this store)
        return id_gen.fetch_add(1, std::memory_order_relaxed);
    }

    static BusMessage make_request(AgentId from, AgentId to,
                                   std::string topic, std::string payload) {
        return {next_id(), MessageType::Request, from, to,
                std::move(topic), std::move(payload)};
    }

    static BusMessage make_response(const BusMessage& req, std::string payload) {
        return {next_id(), MessageType::Response, req.to, req.from,
                req.topic, std::move(payload), req.id};
    }

    static BusMessage make_event(AgentId from, std::string topic,
                                  std::string payload) {
        return {next_id(), MessageType::Event, from, 0,
                std::move(topic), std::move(payload)};
    }
};

} // namespace agentos::bus
