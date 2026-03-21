# P1 Core Capabilities Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add CoExecutor coroutine scheduler, async LLM inference, Ollama/Anthropic/llama.cpp backends with shared HttpClient, and HNSW index persistence.

**Architecture:** Three independent tracks — (A) coroutine infrastructure, (B) LLM backends with shared HTTP layer, (C) HNSW persistence. Track B is subdivided into HttpClient extraction → Ollama → Anthropic → llama.cpp.

**Tech Stack:** C++23 coroutines, libcurl, nlohmann/json, hnswlib, llama.cpp (optional)

**Spec:** `docs/superpowers/specs/2026-03-21-p1-core-capabilities-design.md`

---

## Task 1: CoExecutor Coroutine Scheduler

**Files:**
- Create: `include/agentos/core/co_executor.hpp`
- Create: `src/core/co_executor.cpp`
- Create: `tests/test_coroutine.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `tests/test_coroutine.cpp` with tests for CoExecutor:
- `CoExecutorTest::ScheduleAndRun` — schedule a coroutine handle, verify it executes
- `CoExecutorTest::MultipleCoroutines` — schedule 10 coroutines, verify all execute
- `CoExecutorTest::StopGracefully` — stop executor, verify no crash
- `CoExecutorTest::TaskOnExecutor` — run a `Task<int>` that returns 42 via CoExecutor

- [ ] **Step 2: Implement CoExecutor**

Create `include/agentos/core/co_executor.hpp` and `src/core/co_executor.cpp`:
- Thread pool of `std::jthread` workers consuming from `std::queue<std::coroutine_handle<>>`
- `schedule(handle)`: push to queue, notify one worker
- Workers: wait on condition_variable, pop and `handle.resume()`
- `stop()`: set flag, notify all, join threads
- Destructor calls `stop()`

- [ ] **Step 3: Add to CMakeLists.txt, build and test**

Add `src/core/co_executor.cpp` to `AGENTOS_SOURCES`, `tests/test_coroutine.cpp` to `TEST_SOURCES`.

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='CoExecutor*'`

- [ ] **Step 4: Commit**

```bash
git add include/agentos/core/co_executor.hpp src/core/co_executor.cpp tests/test_coroutine.cpp CMakeLists.txt
git commit -m "feat(coroutine): add CoExecutor thread-pool coroutine scheduler"
```

---

## Task 2: Async LLM Inference + Agent run_co()

**Files:**
- Modify: `include/agentos/kernel/llm_kernel.hpp`
- Modify: `include/agentos/agent.hpp`
- Modify: `include/agentos/agentos.hpp` (add CoExecutor to AgentOS)
- Modify: `tests/test_coroutine.cpp`

- [ ] **Step 1: Write failing tests**

Append to `tests/test_coroutine.cpp`:
- `AsyncInferTest::InferAsyncReturnsMockResponse` — create mock backend + LLMKernel, call `infer_async()`, verify response via `.run()`
- `AsyncInferTest::EmbedAsyncReturnsCachedOrBackend` — call `embed_async()`, verify result

- [ ] **Step 2: Add CoExecutor to AgentOS**

In `include/agentos/agent.hpp`:
- Add `#include <agentos/core/co_executor.hpp>`
- Add `std::unique_ptr<CoExecutor> co_executor_;` member to AgentOS
- Initialize in constructor after scheduler: `co_executor_ = std::make_unique<CoExecutor>(2);`
- In destructor, stop before scheduler: `co_executor_.reset();`
- Add accessor: `CoExecutor& co_executor() { return *co_executor_; }`

- [ ] **Step 3: Add infer_async() and embed_async() to LLMKernel**

In `include/agentos/kernel/llm_kernel.hpp`, add to `LLMKernel`:
```cpp
    Task<Result<LLMResponse>> infer_async(LLMRequest req) {
        // Run synchronous infer() inside the coroutine — the CoExecutor
        // thread handles the blocking curl I/O
        co_return infer(std::move(req));
    }

    Task<Result<EmbeddingResponse>> embed_async(EmbeddingRequest req) {
        co_return embed(std::move(req));
    }
```

These are thin coroutine wrappers. The actual async dispatch to CoExecutor happens at the call site via `co_await`.

- [ ] **Step 4: Build and test**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*Coroutine*:*AsyncInfer*'`

- [ ] **Step 5: Commit**

```bash
git add include/agentos/kernel/llm_kernel.hpp include/agentos/agent.hpp tests/test_coroutine.cpp
git commit -m "feat(coroutine): add infer_async/embed_async and CoExecutor in AgentOS"
```

---

## Task 3: HttpClient Extraction

**Files:**
- Create: `include/agentos/kernel/http_client.hpp`
- Create: `src/kernel/http_client.cpp`
- Modify: `src/kernel/llm_kernel.cpp` (use HttpClient)
- Create: `tests/test_http_client.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `tests/test_http_client.cpp`:
- `HttpClientTest::PostReturnsResponse` — test against a non-existent localhost port, verify returns error (not crash)
- `HttpClientTest::HeadersAreSent` — verify headers struct works
- `HttpClientTest::TimeoutWorks` — set 1s timeout, connect to unresponsive host

- [ ] **Step 2: Create HttpClient**

Create `include/agentos/kernel/http_client.hpp` with `HttpResponse` struct and `HttpClient` class.

Create `src/kernel/http_client.cpp`:
- Extract `CurlHandle` RAII, `CurlHeaders` RAII, `write_string_callback` from `src/kernel/llm_kernel.cpp`
- `curl_global_init` via `static std::once_flag`
- `post()`: standard curl_easy POST with configurable timeout, returns `Result<HttpResponse>`
- `post_stream()`: curl_easy with write callback that delegates raw chunks to `on_data`, returns `Result<HttpResponse>`

- [ ] **Step 3: Refactor OpenAIBackend to use HttpClient**

Modify `src/kernel/llm_kernel.cpp`:
- Replace inline `http_post()` with `HttpClient::post()` call
- Replace stream curl setup with `HttpClient::post_stream()`
- Remove duplicated RAII helpers (now in `http_client.cpp`)
- Keep SSE parsing (`StreamContext`, `process_sse_line`) in `llm_kernel.cpp` — this is OpenAI-specific

- [ ] **Step 4: Build and verify ALL existing tests pass**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos`
Expected: ALL tests pass (this is a refactor, no behavior change)

- [ ] **Step 5: Commit**

```bash
git add include/agentos/kernel/http_client.hpp src/kernel/http_client.cpp src/kernel/llm_kernel.cpp tests/test_http_client.cpp CMakeLists.txt
git commit -m "refactor(kernel): extract shared HttpClient from OpenAIBackend"
```

---

## Task 4: OllamaBackend

**Files:**
- Create: `include/agentos/kernel/ollama_backend.hpp`
- Create: `src/kernel/ollama_backend.cpp`
- Create: `tests/test_ollama_backend.cpp`
- Modify: `include/agentos/agentos.hpp` (builder)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests with mock HttpClient**

Create `tests/test_ollama_backend.cpp`:
- `OllamaBackendTest::CompleteBuildsCorrectRequest` — verify JSON request has `/api/chat` endpoint, `model`, `messages` fields, no auth header
- `OllamaBackendTest::ParsesOllamaResponse` — mock returns Ollama JSON response, verify `LLMResponse.content`, token counts from `eval_count`/`prompt_eval_count`
- `OllamaBackendTest::EmbedUsesCorrectEndpoint` — verify calls `/api/embed`
- `OllamaBackendTest::ParsesToolCalls` — mock returns tool_calls in Ollama format, verify `ToolCallRequest` mapping

To make HttpClient injectable, `OllamaBackend` constructor takes `std::shared_ptr<HttpClient>` (default: create new one).

- [ ] **Step 2: Implement OllamaBackend**

Create header and implementation:
- `complete()`: build Ollama chat JSON → `http_client_->post(base_url_ + "/api/chat", ...)` → parse response
- `stream()`: `post_stream()` with NDJSON line parser (each line is a complete JSON, not SSE)
- `embed()`: build JSON `{model, input}` → `post(base_url_ + "/api/embed", ...)` → parse
- Message format: same as OpenAI (role/content), no system field extraction needed for Ollama

- [ ] **Step 3: Add `.ollama()` to AgentOSBuilder**

In `include/agentos/agentos.hpp`:
```cpp
    AgentOSBuilder& ollama(std::string model = "llama3",
                           std::string base_url = "http://localhost:11434") {
        custom_backend_ = std::make_unique<kernel::OllamaBackend>(
            std::move(model), std::move(base_url));
        backend_type_ = BackendType::Custom;
        return *this;
    }
```

- [ ] **Step 4: Build and test**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='Ollama*'`

- [ ] **Step 5: Commit**

```bash
git add include/agentos/kernel/ollama_backend.hpp src/kernel/ollama_backend.cpp tests/test_ollama_backend.cpp include/agentos/agentos.hpp CMakeLists.txt
git commit -m "feat(kernel): add OllamaBackend for local LLM inference"
```

---

## Task 5: AnthropicBackend

**Files:**
- Create: `include/agentos/kernel/anthropic_backend.hpp`
- Create: `src/kernel/anthropic_backend.cpp`
- Create: `tests/test_anthropic_backend.cpp`
- Modify: `include/agentos/agentos.hpp` (builder)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests with mock HttpClient**

Create `tests/test_anthropic_backend.cpp`:
- `AnthropicBackendTest::RequestHasCorrectHeaders` — verify `x-api-key` and `anthropic-version` headers
- `AnthropicBackendTest::SystemPromptExtracted` — verify system messages become `system` field, not in `messages` array
- `AnthropicBackendTest::ParsesTextResponse` — mock returns Anthropic JSON, verify content extraction
- `AnthropicBackendTest::ParsesToolUseResponse` — mock returns `content: [{type: "tool_use", ...}]`, verify `ToolCallRequest` mapping with `input.dump()` as `args_json`
- `AnthropicBackendTest::ToolUseFinishReasonMapped` — `stop_reason: "end_turn"` with tool_use → `finish_reason = "tool_calls"`
- `AnthropicBackendTest::TokenUsageMapped` — `input_tokens`/`output_tokens` → `prompt_tokens`/`completion_tokens`

- [ ] **Step 2: Implement AnthropicBackend**

Key differences from OpenAI:
- Auth headers: `x-api-key: {key}`, `anthropic-version: 2024-10-22`, `content-type: application/json`
- Request building: extract system messages → `system` field; remaining messages in `messages` array
- Response parsing: iterate `content` array, collect `text` blocks into `content`, `tool_use` blocks into `tool_calls`
- Tool call mapping: `{id, name, input(object)}` → `ToolCallRequest{id, name, input.dump()}`
- Streaming: SSE format (same `data:` prefix as OpenAI), but event types differ — `content_block_delta` has `delta.text`, `message_delta` has `stop_reason`
- `embed()`: not overridden, uses default "not supported" error

- [ ] **Step 3: Add `.anthropic()` to AgentOSBuilder**

```cpp
    AgentOSBuilder& anthropic(std::string api_key,
                              std::string model = "claude-sonnet-4-20250514") {
        custom_backend_ = std::make_unique<kernel::AnthropicBackend>(
            std::move(api_key), std::move(model));
        backend_type_ = BackendType::Custom;
        return *this;
    }
```

- [ ] **Step 4: Build and test**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='Anthropic*'`

- [ ] **Step 5: Commit**

```bash
git add include/agentos/kernel/anthropic_backend.hpp src/kernel/anthropic_backend.cpp tests/test_anthropic_backend.cpp include/agentos/agentos.hpp CMakeLists.txt
git commit -m "feat(kernel): add AnthropicBackend with tool use support"
```

---

## Task 6: LlamaCppBackend (Optional Build)

**Files:**
- Create: `include/agentos/kernel/llamacpp_backend.hpp`
- Create: `src/kernel/llamacpp_backend.cpp`
- Create: `tests/test_llamacpp_backend.cpp`
- Modify: `include/agentos/agentos.hpp` (builder)
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write conditional tests**

Create `tests/test_llamacpp_backend.cpp`:
- Wrap all tests in `#ifdef AGENTOS_ENABLE_LLAMACPP`
- `LlamaCppBackendTest::ConfigDefaults` — verify Config struct defaults
- `LlamaCppBackendTest::NameReturnsLlamacpp` — verify `name()` returns "llamacpp"
- If no model file available, test only construction and config — skip inference tests

When `AGENTOS_ENABLE_LLAMACPP` is not defined, the test file should be empty (just includes, no test cases).

- [ ] **Step 2: Create LlamaCppBackend header (pimpl)**

Create `include/agentos/kernel/llamacpp_backend.hpp` with the `Config` struct and pimpl `Impl`.
Guard the entire file with `#ifdef AGENTOS_ENABLE_LLAMACPP`.

- [ ] **Step 3: Create LlamaCppBackend implementation**

Create `src/kernel/llamacpp_backend.cpp`:
- `Impl` struct holds: `llama_model*`, `llama_context*`, `std::mutex mu_`
- Constructor: `llama_model_params`, `llama_context_params`, load model
- Destructor: `llama_free`, `llama_free_model`
- `complete()`: tokenize messages → `llama_decode` → sample tokens → detokenize
- `stream()`: same but call `cb` after each token
- `embed()`: `llama_decode` with `LLAMA_POOLING_TYPE_MEAN`, extract embeddings
- All methods `std::lock_guard lk(mu_)` for thread safety

- [ ] **Step 4: Add CMake option**

```cmake
option(AGENTOS_ENABLE_LLAMACPP "Enable llama.cpp local inference backend" OFF)
if(AGENTOS_ENABLE_LLAMACPP)
    FetchContent_Declare(llamacpp
        GIT_REPOSITORY https://github.com/ggerganov/llama.cpp.git
        GIT_TAG master
        GIT_SHALLOW TRUE
    )
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LLAMA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(llamacpp)
    target_link_libraries(agentos PUBLIC llama)
    target_compile_definitions(agentos PUBLIC AGENTOS_ENABLE_LLAMACPP=1)
endif()
```

- [ ] **Step 5: Add `.llamacpp()` to AgentOSBuilder**

Guard with `#ifdef AGENTOS_ENABLE_LLAMACPP`.

- [ ] **Step 6: Build and test (without llama.cpp enabled)**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos`
Expected: ALL tests pass (llama.cpp tests are ifdef'd out)

- [ ] **Step 7: Commit**

```bash
git add include/agentos/kernel/llamacpp_backend.hpp src/kernel/llamacpp_backend.cpp tests/test_llamacpp_backend.cpp include/agentos/agentos.hpp CMakeLists.txt
git commit -m "feat(kernel): add LlamaCppBackend with optional build support"
```

---

## Task 7: HNSW Index Persistence

**Files:**
- Modify: `include/agentos/memory/memory.hpp`
- Modify: `src/memory/memory.cpp`
- Create: `tests/test_hnsw_persistence.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create `tests/test_hnsw_persistence.cpp`:
- `ShortTermMemoryPersistenceTest::SaveAndLoadRoundTrip`:
  1. Create ShortTermMemory with persist_dir
  2. Write 5 entries with embeddings
  3. Call `save(dir)`
  4. Create new ShortTermMemory, call `load(dir)`
  5. Verify all 5 entries retrievable via `read()`
  6. Verify `search()` returns same results as before save
- `ShortTermMemoryPersistenceTest::LoadMissingFilesStartsFresh`:
  1. Create ShortTermMemory, call `load("/nonexistent")`
  2. Verify no error, empty store
- `ShortTermMemoryPersistenceTest::DirtyFlagTracking`:
  1. Create STM, verify not dirty
  2. Write entry, verify dirty
  3. Save, verify not dirty

- [ ] **Step 2: Add save/load to ShortTermMemory**

In `include/agentos/memory/memory.hpp`, add to ShortTermMemory:
- `std::string persist_dir_` member
- `bool dirty_{false}` flag
- `Result<void> save(const std::string& dir)`:
  - Lock `mu_`
  - If not dirty, return early
  - Save HNSW index to `dir/stm_hnsw.bin` (temp file + rename)
  - Save metadata to `dir/stm_metadata.jsonl`: one JSON per entry from `store_`, plus `label_counter_` and maps
  - Set `dirty_ = false`
- `Result<void> load(const std::string& dir)`:
  - Lock `mu_`
  - If `dir/stm_hnsw.bin` doesn't exist, return success (fresh start)
  - `hnsw_index_->loadIndex(path, capacity_)`
  - Parse `stm_metadata.jsonl` to rebuild `store_`, `id_to_label_`, `label_to_id_`, `label_counter_`
  - Set `dirty_ = false`
- Set `dirty_ = true` in `write()` and `forget()`

- [ ] **Step 3: Add save_indexes/load_indexes to MemorySystem**

In `include/agentos/memory/memory.hpp`, add to MemorySystem:
- `save_indexes()`: calls `short_term_->save(ltm_dir_)` — catch exceptions, log errors
- `load_indexes()`: calls `short_term_->load(ltm_dir_)`
- Call `load_indexes()` at end of constructor
- Call `save_indexes()` in destructor (noexcept — catch and log)

- [ ] **Step 4: Build and test**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos --gtest_filter='*Persistence*'`
Then full suite.

- [ ] **Step 5: Commit**

```bash
git add include/agentos/memory/memory.hpp src/memory/memory.cpp tests/test_hnsw_persistence.cpp CMakeLists.txt
git commit -m "feat(memory): add HNSW index persistence for ShortTermMemory"
```

---

## Task 8: Full Integration Test

- [ ] **Step 1: Run full test suite**

Run: `cd build && cmake .. && make -j$(nproc) test_agentos && ./test_agentos`
Expected: ALL tests pass

- [ ] **Step 2: Verify zero warnings**

Run: `make -j$(nproc) test_agentos 2>&1 | grep -i "warning:" | grep -v "_deps" | head -20`
Expected: No warnings from agentos sources

- [ ] **Step 3: Commit**

```bash
git commit --allow-empty -m "test: verify P1 core capabilities integration — all tests pass"
```
