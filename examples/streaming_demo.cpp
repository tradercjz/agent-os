// ============================================================
// AgentOS Streaming Demo
// Demonstrates SSE (Server-Sent Events) streaming via the API
// server, including StreamWriter usage and SSE event formatting.
// ============================================================
//
// To test with curl (after running this program):
//   curl -N -X POST http://localhost:9091/api/v1/infer/stream \
//     -H "Content-Type: application/json" \
//     -d '{"messages":[{"role":"user","content":"Hello, stream me a response!"}]}'
//
// The -N flag disables curl's output buffering so you see
// tokens arrive in real time.
//
// ============================================================
#include <agentos/agentos.hpp>
#include <agentos/api/streaming.hpp>
#include <iostream>
#include <sstream>

using namespace agentos;
using namespace agentos::api;
using namespace agentos::kernel;

// ── Helper formatting ──────────────────────────────────────

void section(std::string_view title) {
    std::cout << "\n== " << title << " ==\n";
}

void info(std::string_view msg) {
    std::cout << "  " << msg << "\n";
}

// ── Part 1: Demonstrate SSE event formatting ───────────────

void demo_sse_event_format() {
    section("1. SSE Event Formatting");
    info("StreamEvent produces spec-compliant SSE text.\n");

    // A token event carries one chunk of generated text.
    StreamEvent token_ev = make_token_event("Hello");
    std::cout << "  Token event (raw):\n";
    // Show each line prefixed with "    | " for clarity
    std::istringstream iss(token_ev.format());
    std::string line;
    while (std::getline(iss, line)) {
        std::cout << "    | " << line << "\n";
    }

    // A done event signals the end of generation with usage stats.
    StreamEvent done_ev = make_done_event(/*prompt_tokens=*/12,
                                           /*completion_tokens=*/5);
    std::cout << "\n  Done event (raw):\n";
    std::istringstream iss2(done_ev.format());
    while (std::getline(iss2, line)) {
        std::cout << "    | " << line << "\n";
    }

    // An error event reports a problem to the client.
    StreamEvent err_ev = make_error_event("Rate limit exceeded");
    std::cout << "\n  Error event (raw):\n";
    std::istringstream iss3(err_ev.format());
    while (std::getline(iss3, line)) {
        std::cout << "    | " << line << "\n";
    }

    // Events can carry optional IDs for client reconnection.
    StreamEvent ev_with_id;
    ev_with_id.event = "token";
    ev_with_id.data = R"({"token":" world"})";
    ev_with_id.id = "evt-42";
    std::cout << "\n  Event with ID (for reconnection):\n";
    std::istringstream iss4(ev_with_id.format());
    while (std::getline(iss4, line)) {
        std::cout << "    | " << line << "\n";
    }
}

// ── Part 2: Demonstrate StreamWriter with a pipe ───────────

void demo_stream_writer() {
    section("2. StreamWriter (local pipe demo)");
    info("StreamWriter writes chunked HTTP + SSE to a file descriptor.");
    info("Here we use a pipe to capture the output locally.\n");

    // Create a pipe so we can capture what StreamWriter produces.
    int fds[2];
    if (pipe(fds) != 0) {
        std::cerr << "  pipe() failed\n";
        return;
    }
    int read_fd = fds[0];
    int write_fd = fds[1];

    // StreamWriter takes ownership of the write-side fd.
    StreamWriter writer(write_fd);

    // Step 1: Send SSE HTTP headers (Content-Type: text/event-stream)
    auto r = writer.begin();
    if (!r.has_value()) {
        std::cerr << "  begin() failed: " << r.error().message << "\n";
        close(read_fd);
        close(write_fd);
        return;
    }
    info("begin() sent HTTP headers for SSE.");

    // Step 2: Send a few token events (simulating LLM output)
    std::vector<std::string> tokens = {"The", " quick", " brown", " fox"};
    for (size_t i = 0; i < tokens.size(); ++i) {
        auto ev = make_token_event(tokens[i], "evt-" + std::to_string(i));
        (void)writer.send(ev);
    }
    info("Sent 4 token events.");

    // Step 3: Send the done event and close
    (void)writer.send(make_done_event(10, 4));
    (void)writer.end();
    info("Sent done event and terminated stream.");

    // Read what was written to the pipe
    close(write_fd);  // close write end so read doesn't block
    std::string captured;
    char buf[4096];
    ssize_t n;
    while ((n = read(read_fd, buf, sizeof(buf))) > 0) {
        captured.append(buf, static_cast<size_t>(n));
    }
    close(read_fd);

    std::cout << "\n  Bytes written: " << captured.size() << "\n";
    std::cout << "  Stream is_open after end(): "
              << (writer.is_open() ? "true" : "false") << "\n";

    // Show a snippet of the captured output (the HTTP headers)
    auto header_end = captured.find("\r\n\r\n");
    if (header_end != std::string::npos) {
        std::string headers = captured.substr(0, header_end);
        std::cout << "\n  Captured HTTP headers:\n";
        std::istringstream hss(headers);
        std::string hline;
        while (std::getline(hss, hline)) {
            // Remove trailing \r
            if (!hline.empty() && hline.back() == '\r') hline.pop_back();
            std::cout << "    | " << hline << "\n";
        }
    }
}

// ── Part 3: Full API server with streaming endpoint ────────

void demo_api_server_streaming() {
    section("3. API Server with Streaming Endpoint");

    // Build an AgentOS with a mock backend that has a custom rule.
    auto mock = std::make_unique<MockLLMBackend>("stream-demo-model");
    mock->register_rule(
        "stream",
        "Streaming is a technique where the server sends data "
        "incrementally as it becomes available, rather than waiting "
        "for the complete response. This reduces perceived latency "
        "and improves the user experience.",
        /*priority=*/10);

    auto os = AgentOSBuilder()
                  .backend(std::move(mock))
                  .security(false)
                  .threads(2)
                  .build();

    // Start the API server on port 9091
    auto r = os->start_api(9091);
    if (!r.has_value()) {
        std::cerr << "  Failed to start API: " << r.error().message << "\n";
        info("(Port 9091 may be in use. This is fine for the demo.)");
        info("Parts 1 and 2 above demonstrated the streaming primitives.");
        return;
    }

    std::cout << "  API server running at http://localhost:9091\n\n";
    std::cout << "  The streaming endpoint is:\n";
    std::cout << "    POST /api/v1/infer/stream\n\n";
    std::cout << "  Try it with curl:\n";
    std::cout << R"(    curl -N -X POST http://localhost:9091/api/v1/infer/stream \)" << "\n";
    std::cout << R"(      -H "Content-Type: application/json" \)" << "\n";
    std::cout << R"(      -d '{"messages":[{"role":"user","content":"Tell me about streaming"}]}')" << "\n";
    std::cout << "\n";
    std::cout << "  Expected SSE output:\n";
    std::cout << "    event: token\n";
    std::cout << R"(    data: {"token":"Streaming"})" << "\n";
    std::cout << "    \n";
    std::cout << "    event: token\n";
    std::cout << R"(    data: {"token":" is"})" << "\n";
    std::cout << "    ...\n";
    std::cout << "    event: done\n";
    std::cout << R"(    data: {"status":"done","tokens":{"prompt":...,"completion":...}})" << "\n";
    std::cout << "\n";

    // Also show that regular (non-streaming) inference still works:
    info("Regular (non-streaming) inference also works on /api/v1/infer:");
    std::cout << R"(    curl -X POST http://localhost:9091/api/v1/infer \)" << "\n";
    std::cout << R"(      -d '{"messages":[{"role":"user","content":"Hello"}]}')" << "\n";
    std::cout << "\n";

    std::cout << "  Press Enter to stop the server...\n";
    std::cin.get();

    // Server stops automatically when `os` goes out of scope.
}

// ── Main ───────────────────────────────────────────────────

int main() {
    std::cout << "============================================\n";
    std::cout << " AgentOS Streaming Demo\n";
    std::cout << "============================================\n";

    // Part 1: Show how SSE events are formatted
    demo_sse_event_format();

    // Part 2: Show StreamWriter writing to a local pipe
    demo_stream_writer();

    // Part 3: Run a full API server with streaming
    demo_api_server_streaming();

    std::cout << "\nStreaming demo complete.\n";
    return 0;
}
