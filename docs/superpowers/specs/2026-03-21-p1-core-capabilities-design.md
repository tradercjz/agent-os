# P1 Core Capabilities Design Spec

**Date:** 2026-03-21
**Batch:** P1 — Core Capability Enhancements
**Scope:** C++20 Coroutines, LLM Backend Expansion (Ollama/Anthropic/llama.cpp), HNSW Index Persistence

**Note:** Embedding Cache was already implemented in `LLMKernel` (`EmbeddingCache` class in `llm_kernel.hpp`). Removed from P1 scope.

---

## 1. C++20 Coroutine Infrastructure

### Current State

`LLMKernel::infer()` is synchronous. `Agent::run_async()` returns `std::future` via thread pool dispatch. Each concurrent LLM call consumes a scheduler thread blocking on curl I/O.

### Design

**Goal:** Introduce a `Task<T>` coroutine type and async LLM inference path. Keep existing synchronous API intact.

#### 1.1 Task<T> Coroutine Type

```cpp
// include/agentos/core/task.hpp
namespace agentos {

template<typename T>
class Task {
public:
    struct promise_type { ... };
    using handle_type = std::coroutine_handle<promise_type>;

    // co_await support
    bool await_ready() const noexcept;
    void await_suspend(std::coroutine_handle<> caller);
    T await_resume();

    // Blocking bridge for non-coroutine callers
    T run();   // NOTE: existing task.hpp uses run(), not get()

    // Check if result is ready
    bool done() const noexcept;  // NOTE: existing task.hpp uses done(), not is_ready()
};

// Void specialization — already implemented in task.hpp
template<>
class Task<void> { /* ... already exists ... */ };

} // namespace agentos
```

**Note:** `Task<T>` and `Task<void>` already exist in `include/agentos/core/task.hpp` with `run()` and `done()` methods. No new implementation needed — only the `CoExecutor` and async wrappers are new.

**Key design decisions:**
- `promise_type` stores `Result<T>` for the return value or exception
- Lazy start: coroutine doesn't run until awaited or `run()` called
- `run()` blocks current thread (bridge for existing synchronous API)
- `await_suspend` stores the continuation and resumes it when value is ready

#### 1.2 CoExecutor — Simple Coroutine Scheduler

```cpp
// include/agentos/core/co_executor.hpp
namespace agentos {

class CoExecutor {
public:
    explicit CoExecutor(size_t threads = 1);
    ~CoExecutor();

    // Schedule a coroutine for execution
    void schedule(std::coroutine_handle<> h);

    // Run the event loop (blocking)
    void run();

    // Stop the executor
    void stop();

private:
    std::queue<std::coroutine_handle<>> ready_queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::vector<std::jthread> workers_;
    bool stop_{false};
};

} // namespace agentos
```

A simple thread-pool-based executor that runs ready coroutines. Not a full async I/O runtime — just dispatches coroutine continuations.

**Lifetime:** `CoExecutor` is owned by `AgentOS` as a member. Initialized after `Scheduler`, stopped during `graceful_shutdown()` before scheduler shutdown. The executor uses a separate thread pool from the scheduler (default 2 threads) to avoid contention with task scheduling.

**Threading model:** `infer_async()` dispatches the coroutine onto the `CoExecutor` thread pool. Each coroutine blocks its executor thread during curl I/O (same as current `infer()`). The benefit is that callers can `co_await` without occupying a scheduler thread — the scheduler threads remain free for task dispatch. True non-blocking I/O via `curl_multi` is deferred to a future iteration.

#### 1.3 Async LLM Inference

```cpp
// In LLMKernel:
Task<Result<LLMResponse>> infer_async(LLMRequest req);
Task<Result<EmbeddingResponse>> embed_async(EmbeddingRequest req);
```

Implementation: wraps `infer()` in a `Task` that runs on the `CoExecutor`. The curl call itself remains synchronous within the coroutine — the benefit is that callers can `co_await` without blocking a scheduler thread.

Future optimization: replace curl_easy with curl_multi for true non-blocking I/O (out of P1 scope).

#### 1.4 Agent Coroutine Support

```cpp
// In Agent:
Task<std::string> run_co(const std::string& input);
```

The ReAct loop becomes:
```cpp
Task<Result<std::string>> ReActAgent::run_co(const std::string& input) {
    for (int step = 0; step < max_steps_; ++step) {
        auto response = co_await os_->kernel().infer_async(req);
        if (!response.has_value()) {
            co_return make_error(response.error().code, response.error().message);
        }
        if (response->wants_tool_call()) {
            auto tool_result = dispatch_tool(response->tool_calls);
            // ... add tool result to context ...
        } else {
            co_return response->content;
        }
    }
    co_return make_error(ErrorCode::Timeout, "max steps reached");
}
```

Existing `run()` and `run_async()` stay as-is for backward compatibility.

#### 1.5 Files

| File | Action |
|------|--------|
| `include/agentos/core/task.hpp` | **New** — `Task<T>` coroutine type |
| `include/agentos/core/co_executor.hpp` | **New** — coroutine scheduler |
| `src/core/co_executor.cpp` | **New** — executor implementation |
| `include/agentos/kernel/llm_kernel.hpp` | **Modify** — add `infer_async()`, `embed_async()` |
| `include/agentos/agent.hpp` | **Modify** — add `run_co()` to Agent/ReActAgent |
| `tests/test_coroutine.cpp` | **New** — Task<T>, CoExecutor, async infer tests |

---

## 2. LLM Backend Expansion

### Current State

Two backends: `MockLLMBackend` (testing) and `OpenAIBackend` (production, curl-based).

`ILLMBackend` interface:
```cpp
class ILLMBackend {
    virtual Result<LLMResponse> complete(const LLMRequest& req) = 0;
    virtual Result<EmbeddingResponse> embed(const EmbeddingRequest& req) = 0;  // default: not supported
    virtual Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) = 0;  // default: complete()
    virtual std::string name() const = 0;
};
```

### 2.1 OllamaBackend

**Rationale:** Ollama's API is nearly identical to OpenAI's chat completions format. Minimal code.

```cpp
// include/agentos/kernel/ollama_backend.hpp
namespace agentos::kernel {

class OllamaBackend : public ILLMBackend {
public:
    explicit OllamaBackend(std::string model = "llama3",
                           std::string base_url = "http://localhost:11434");
    Result<LLMResponse> complete(const LLMRequest& req) override;
    Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    Result<EmbeddingResponse> embed(const EmbeddingRequest& req) override;
    std::string name() const override { return "ollama"; }
private:
    std::string model_;
    std::string base_url_;
    // Reuses OpenAIBackend's curl/JSON infrastructure via shared helpers
};
```

**API differences from OpenAI:**
- Endpoint: `/api/chat` (not `/v1/chat/completions`)
- No auth header needed (local)
- Response: `{"message": {"role": "assistant", "content": "..."}, "done": true, "eval_count": N, "prompt_eval_count": M}` — no `choices` array
- Token usage: `eval_count` → completion_tokens, `prompt_eval_count` → prompt_tokens
- Tool calling (Ollama 0.3+): `message.tool_calls` array (similar to OpenAI format), `done_reason: "stop"`
- Embedding endpoint: `/api/embed` (not `/api/embeddings` — singular is current, plural is deprecated)
- Streaming: NDJSON (newline-delimited JSON, not SSE `data:` prefix) — each line is a complete JSON object

**Implementation:** Extract shared curl helpers (`http_post`, `CurlHandle`, `CurlHeaders`) from `llm_kernel.cpp` into `src/kernel/http_client.cpp`. Both OpenAI and Ollama backends reuse them.

### 2.2 AnthropicBackend

```cpp
// include/agentos/kernel/anthropic_backend.hpp
namespace agentos::kernel {

class AnthropicBackend : public ILLMBackend {
public:
    explicit AnthropicBackend(std::string api_key,
                              std::string model = "claude-sonnet-4-20250514",
                              std::string base_url = "https://api.anthropic.com");
    Result<LLMResponse> complete(const LLMRequest& req) override;
    Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    std::string name() const override { return "anthropic"; }
    // embed() uses default (not supported) — Anthropic has no embedding API
private:
    std::string api_key_;
    std::string model_;
    std::string base_url_;
};
```

**API differences from OpenAI:**
- Auth: `x-api-key` header (not `Authorization: Bearer`)
- Version header: `anthropic-version: 2024-10-22` (configurable, use latest stable)
- System prompt: separate `system` field (not a message with role "system")
- Request body: `{ model, max_tokens, system, messages: [{role, content}] }`
- Response: `{ content: [{type: "text", text: "..."}], stop_reason, usage: {input_tokens, output_tokens} }`
- Streaming: SSE with event types `content_block_start`, `content_block_delta`, `message_delta`

**Tool use mapping to ToolCallRequest:**
- Anthropic returns `content: [{type: "text", text: "..."}, {type: "tool_use", id, name, input: {...}}]`
- `text` blocks → concatenate into `LLMResponse::content`
- `tool_use` blocks → map to `ToolCallRequest{.id = id, .name = name, .args_json = input.dump()}`
  - Note: Anthropic's `input` is a JSON object, must be serialized to string for `args_json`
- `stop_reason: "end_turn"` with `tool_use` blocks present → map to `finish_reason = "tool_calls"` for compatibility with existing `wants_tool_call()` logic
- Tool results sent back as: `{role: "user", content: [{type: "tool_result", tool_use_id, content: "..."}]}`

**Embedding limitation:** Anthropic has no embedding API. `embed()` returns default error. If users need embeddings with Anthropic chat, they should configure a separate embedding backend (e.g., OpenAI) — future multi-backend support. For P1, this is documented as a known limitation.

### 2.3 LlamaCppBackend

```cpp
// include/agentos/kernel/llamacpp_backend.hpp
namespace agentos::kernel {

class LlamaCppBackend : public ILLMBackend {
public:
    struct Config {
        std::string model_path;       // path to GGUF model file
        int n_ctx{4096};              // context window
        int n_gpu_layers{0};          // GPU offload layers (0 = CPU only)
        int n_threads{4};             // inference threads
    };

    explicit LlamaCppBackend(Config config);
    ~LlamaCppBackend();
    Result<LLMResponse> complete(const LLMRequest& req) override;
    Result<LLMResponse> stream(const LLMRequest& req, TokenCallback cb) override;
    Result<EmbeddingResponse> embed(const EmbeddingRequest& req) override;
    std::string name() const override { return "llamacpp"; }

private:
    struct Impl; // pimpl to hide llama.h dependency
    std::unique_ptr<Impl> impl_;
};

} // namespace agentos::kernel
```

**Build integration:**
- Optional: `cmake -DAGENTOS_ENABLE_LLAMACPP=ON`
- FetchContent from `https://github.com/ggerganov/llama.cpp` with `BUILD_SHARED_LIBS OFF`, only link `llama` target
- Compile-guarded with `#ifdef AGENTOS_ENABLE_LLAMACPP`
- Pimpl pattern to isolate llama.h headers from public API

**Thread safety:** `llama_context` is NOT thread-safe for concurrent inference. `Impl` holds a `std::mutex` and serializes all `complete()`/`stream()`/`embed()` calls. Consistent with project's `std::lock_guard` pattern.

### 2.4 AgentOSBuilder Integration

```cpp
// In AgentOSBuilder:
AgentOSBuilder& ollama(std::string model = "llama3",
                       std::string base_url = "http://localhost:11434");
AgentOSBuilder& anthropic(std::string api_key,
                          std::string model = "claude-sonnet-4-20250514");
AgentOSBuilder& llamacpp(LlamaCppBackend::Config config);
```

### 2.5 Shared HTTP Client Extraction

Extract from `src/kernel/llm_kernel.cpp` into reusable module:

```cpp
// include/agentos/kernel/http_client.hpp
namespace agentos::kernel {

struct HttpResponse {
    long status_code;
    std::string body;
    std::string error;
};

class HttpClient {
public:
    HttpClient();  // calls curl_global_init via std::once_flag

    Result<HttpResponse> post(const std::string& url,
                              const std::string& body,
                              const std::vector<std::string>& headers,
                              int timeout_sec = 60);

    // on_data delivers raw bytes — each backend handles its own framing:
    //   OpenAI/Anthropic: SSE parsing (data: prefix)
    //   Ollama: NDJSON parsing (one JSON per line)
    Result<HttpResponse> post_stream(const std::string& url,
                                     const std::string& body,
                                     const std::vector<std::string>& headers,
                                     std::function<void(std::string_view)> on_data,
                                     int timeout_sec = 120);
};

} // namespace agentos::kernel
```

**Design notes:**
- `HttpClient` constructor uses `static std::once_flag` for `curl_global_init()` — safe for multi-instance
- SSE/NDJSON framing is NOT in `HttpClient` — each backend handles its own protocol
- Timeout is configurable per-call (llama.cpp local inference may need longer)
- RAII `CurlHandle` and `CurlHeaders` extracted from `llm_kernel.cpp` anonymous namespace
```

### 2.6 Files

| File | Action |
|------|--------|
| `include/agentos/kernel/http_client.hpp` | **New** — shared HTTP client |
| `src/kernel/http_client.cpp` | **New** — curl implementation extracted |
| `include/agentos/kernel/ollama_backend.hpp` | **New** — Ollama backend |
| `src/kernel/ollama_backend.cpp` | **New** — Ollama implementation |
| `include/agentos/kernel/anthropic_backend.hpp` | **New** — Anthropic backend |
| `src/kernel/anthropic_backend.cpp` | **New** — Anthropic implementation |
| `include/agentos/kernel/llamacpp_backend.hpp` | **New** — llama.cpp backend |
| `src/kernel/llamacpp_backend.cpp` | **New** — llama.cpp implementation |
| `src/kernel/llm_kernel.cpp` | **Modify** — extract HTTP helpers, use HttpClient |
| `include/agentos/agentos.hpp` | **Modify** — add builder methods |
| `CMakeLists.txt` | **Modify** — add sources, optional llama.cpp |
| `tests/test_ollama_backend.cpp` | **New** — Ollama tests: JSON serialization/parsing with injectable HttpClient |
| `tests/test_anthropic_backend.cpp` | **New** — Anthropic tests: message format, tool use mapping, with injectable HttpClient |
| `tests/test_llamacpp_backend.cpp` | **New** — llama.cpp tests (conditional `#ifdef AGENTOS_ENABLE_LLAMACPP`) |

**Test strategy:** `HttpClient` is passed as constructor parameter to each backend, allowing injection of a mock that returns canned responses. Tests verify JSON request building and response parsing without network access.

---

## 3. HNSW Index Persistence

### Current State

`ShortTermMemory` and `LongTermMemory` use hnswlib HNSW indexes in-memory. `LongTermMemory` has partial persistence (index + metadata files) but `ShortTermMemory` rebuilds on every restart. The HNSW index for L1 is lost on process exit.

### Design

**Goal:** Save/load HNSW indexes to disk using hnswlib native `saveIndex()`/`loadIndex()`.

#### 3.1 ShortTermMemory Persistence

Add `dir` parameter to `ShortTermMemory` constructor for persistence path:

```cpp
    explicit ShortTermMemory(size_t capacity = 512,
                             const std::string& persist_dir = "");

    // Save HNSW index + metadata to disk
    Result<void> save(const std::string& dir);

    // Load HNSW index + metadata from disk
    Result<void> load(const std::string& dir);
```

**Two files are persisted:**
1. `{dir}/stm_hnsw.bin` — HNSW graph + vectors via `saveIndex()`/`loadIndex()`
2. `{dir}/stm_metadata.bin` — application-level metadata:
   - `store_` map (all `MemoryEntry` objects, serialized as JSON Lines)
   - `id_to_label_` / `label_to_id_` bidirectional maps
   - `label_counter_` value

**Why two files:** `hnswlib::saveIndex()` only stores the graph structure and vectors. It does NOT store string IDs, importance scores, tags, or other `MemoryEntry` fields. Without the metadata file, loaded vectors cannot be mapped back to entries.

Implementation:
- `save()`: holds `mu_` lock, saves HNSW to temp file + rename (disk-full safety), then writes metadata. `noexcept`-safe for destructor use (errors logged, not thrown).
- `load()`: holds `mu_` lock, loads HNSW then metadata. Rebuilds `id_to_label_`/`label_to_id_` maps. If files don't exist, silently starts with empty index.
- Thread safety: both `save()` and `load()` acquire `mu_` to prevent concurrent modification.
- Dirty flag: `dirty_` bool tracks whether save is needed (set on write/forget, cleared on save).

#### 3.2 MemorySystem Integration

```cpp
class MemorySystem {
public:
    // Save all indexes to disk
    Result<void> save_indexes(const std::string& dir);

    // Load indexes from disk (called during construction)
    void load_indexes(const std::string& dir);
};
```

- `MemorySystem` constructor passes `ltm_dir` to `ShortTermMemory` for persistence
- `MemorySystem` constructor calls `load_indexes(ltm_dir_)` after creating L1/L2
- `MemorySystem` destructor calls `save_indexes(ltm_dir_)` — `noexcept`, errors logged not thrown
- `MemorySystem::flush()` also calls `save_indexes()` for explicit save
- `LongTermMemory` already has partial persistence — extend to use consistent path convention

#### 3.3 Files

| File | Action |
|------|--------|
| `include/agentos/memory/memory.hpp` | **Modify** — add save/load to ShortTermMemory, MemorySystem |
| `tests/test_memory_system.cpp` | **Modify** — add persistence round-trip test |

---

## Summary of All New/Modified Files

### New Files (11)
1. `include/agentos/core/task.hpp`
2. `include/agentos/core/co_executor.hpp`
3. `src/core/co_executor.cpp`
4. `include/agentos/kernel/http_client.hpp`
5. `src/kernel/http_client.cpp`
6. `include/agentos/kernel/ollama_backend.hpp`
7. `src/kernel/ollama_backend.cpp`
8. `include/agentos/kernel/anthropic_backend.hpp`
9. `src/kernel/anthropic_backend.cpp`
10. `include/agentos/kernel/llamacpp_backend.hpp`
11. `src/kernel/llamacpp_backend.cpp`

### New Test Files (4)
12. `tests/test_coroutine.cpp`
13. `tests/test_ollama_backend.cpp`
14. `tests/test_anthropic_backend.cpp`
15. `tests/test_llamacpp_backend.cpp`

### Modified Files (6)
16. `include/agentos/kernel/llm_kernel.hpp` — async methods, EmbeddingCache already exists
17. `include/agentos/agent.hpp` — `run_co()` method
18. `include/agentos/agentos.hpp` — builder methods for new backends
19. `include/agentos/memory/memory.hpp` — HNSW save/load
20. `src/kernel/llm_kernel.cpp` — extract HTTP client, use shared code
21. `CMakeLists.txt` — new sources, optional llama.cpp

### Backward Compatibility
- All existing `infer()`, `run()`, `run_async()` APIs unchanged
- New `infer_async()`, `run_co()` are additive
- `AgentOSBuilder` gains `.ollama()`, `.anthropic()`, `.llamacpp()` — all optional
- `llama.cpp` behind compile flag — zero impact when disabled
- HNSW load is best-effort (missing file = fresh index)
