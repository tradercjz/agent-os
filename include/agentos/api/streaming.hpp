#pragma once
// ============================================================
// AgentOS :: API — Server-Sent Events (SSE) streaming support
// ============================================================
#include <agentos/core/types.hpp>
#include <agentos/core/logger.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <unistd.h>
#include <sys/socket.h>
#endif

namespace agentos::api {

// ── SSE event structure ─────────────────────────────────────

struct StreamEvent {
    std::string event;                  // event type: "token", "done", "error"
    std::string data;                   // event payload (typically JSON)
    std::optional<std::string> id;      // optional event ID for reconnection

    // Format a single SSE event per the spec:
    //   event: <type>\n
    //   id: <id>\n        (if present)
    //   data: <line>\n    (per line of data)
    //   \n
    [[nodiscard]] std::string format() const {
        std::string out;
        if (!event.empty()) {
            out += "event: " + event + "\n";
        }
        if (id.has_value() && !id->empty()) {
            out += "id: " + *id + "\n";
        }
        // data field: split on newlines so each line gets its own "data:" prefix
        if (data.empty()) {
            out += "data: \n";
        } else {
            std::istringstream iss(data);
            std::string line;
            while (std::getline(iss, line)) {
                out += "data: " + line + "\n";
            }
        }
        out += "\n"; // blank line terminates event
        return out;
    }
};

// ── SSE stream writer ───────────────────────────────────────

class StreamWriter : private NonCopyable {
public:
    explicit StreamWriter(int fd) : fd_(fd) {}

    StreamWriter(StreamWriter&& other) noexcept
        : fd_(other.fd_), open_(other.open_), bytes_written_(other.bytes_written_) {
        other.fd_ = -1;
        other.open_ = false;
    }

    StreamWriter& operator=(StreamWriter&& other) noexcept {
        if (this != &other) {
            fd_ = other.fd_;
            open_ = other.open_;
            bytes_written_ = other.bytes_written_;
            other.fd_ = -1;
            other.open_ = false;
        }
        return *this;
    }

    // Write the SSE HTTP headers to begin streaming.
    // Must be called once before sending any events.
    Result<void> begin() {
        if (fd_ < 0) {
            return make_error(ErrorCode::InvalidArgument, "StreamWriter: invalid fd");
        }
        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "Connection: keep-alive\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "\r\n";
        auto r = write_raw(headers);
        if (!r.has_value()) return r;
        open_ = true;
        return {};
    }

    // Send a single SSE event
    Result<void> send(const StreamEvent& ev) {
        if (!open_) {
            return make_error(ErrorCode::InvalidArgument,
                              "StreamWriter: not open (call begin() first)");
        }
        std::string formatted = ev.format();
        // Chunked transfer encoding: hex size + \r\n + data + \r\n
        std::string chunk = format_chunk(formatted);
        return write_raw(chunk);
    }

    // Send the terminating chunk and close
    Result<void> end() {
        if (!open_) return {};
        // Send zero-length chunk to signal end of chunked transfer
        auto r = write_raw("0\r\n\r\n");
        open_ = false;
        return r;
    }

    bool is_open() const noexcept { return open_; }
    int fd() const noexcept { return fd_; }
    size_t bytes_written() const noexcept { return bytes_written_; }

private:
    Result<void> write_raw(const std::string& data) {
        const char* ptr = data.data();
        size_t remaining = data.size();
        while (remaining > 0) {
#ifdef _WIN32
            int n = ::send(fd_, ptr, static_cast<int>(remaining), 0);
#else
            ssize_t n = ::write(fd_, ptr, remaining);
#endif
            if (n <= 0) {
                open_ = false;
                return make_error(ErrorCode::Unknown, "StreamWriter: write failed");
            }
            ptr += n;
            remaining -= static_cast<size_t>(n);
            bytes_written_ += static_cast<size_t>(n);
        }
        return {};
    }

    static std::string format_chunk(const std::string& data) {
        std::ostringstream oss;
        oss << std::hex << data.size() << "\r\n" << data << "\r\n";
        return oss.str();
    }

    int fd_{-1};
    bool open_{false};
    size_t bytes_written_{0};
};

// ── StreamResponse: signals that handle_client should stream ──

struct StreamResponse {
    bool is_stream{true};
    // The handler writes directly via StreamWriter;
    // this struct is returned to indicate "already handled"
    int status{200};
    std::string error_message;   // non-empty if pre-stream error

    static StreamResponse ok() { return {true, 200, {}}; }
    static StreamResponse error(int code, const std::string& msg) {
        return {true, code, msg};
    }
};

// ── Convenience helpers ─────────────────────────────────────

// Build a token event
inline StreamEvent make_token_event(const std::string& token,
                                    const std::string& id = {}) {
    nlohmann::json j;
    j["token"] = token;
    StreamEvent ev;
    ev.event = "token";
    ev.data = j.dump();
    if (!id.empty()) ev.id = id;
    return ev;
}

// Build a done event
inline StreamEvent make_done_event(uint32_t prompt_tokens = 0,
                                   uint32_t completion_tokens = 0) {
    nlohmann::json j;
    j["status"] = "done";
    j["tokens"]["prompt"] = prompt_tokens;
    j["tokens"]["completion"] = completion_tokens;
    StreamEvent ev;
    ev.event = "done";
    ev.data = j.dump();
    return ev;
}

// Build an error event
inline StreamEvent make_error_event(const std::string& message) {
    nlohmann::json j;
    j["error"] = message;
    StreamEvent ev;
    ev.event = "error";
    ev.data = j.dump();
    return ev;
}

} // namespace agentos::api
