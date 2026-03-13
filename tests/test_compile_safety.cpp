#include <agentos/agent.hpp>
#include <agentos/kernel/llm_kernel.hpp>
#include <agentos/memory/memory.hpp>
#include <gtest/gtest.h>
#include <type_traits>

using namespace agentos;
using namespace agentos::kernel;
using namespace agentos::memory;

// ── Concept Verification ───────────────────────

TEST(CompileSafetyTest, AgentConceptVerification) {
  // ReActAgent should satisfy AgentConcept
  static_assert(AgentConcept<ReActAgent>, "ReActAgent must satisfy AgentConcept");
  SUCCEED();
}

TEST(CompileSafetyTest, LLMBackendConceptVerification) {
  // MockLLMBackend should satisfy LLMBackendConcept
  static_assert(LLMBackendConcept<MockLLMBackend>, "MockLLMBackend must satisfy LLMBackendConcept");
  static_assert(LLMBackendConcept<OpenAIBackend>, "OpenAIBackend must satisfy LLMBackendConcept");
  SUCCEED();
}

TEST(CompileSafetyTest, MemoryStoreConceptVerification) {
  // All memory implementations should satisfy MemoryStoreConcept
  static_assert(MemoryStoreConcept<WorkingMemory>, "WorkingMemory must satisfy MemoryStoreConcept");
  static_assert(MemoryStoreConcept<ShortTermMemory>, "ShortTermMemory must satisfy MemoryStoreConcept");
  static_assert(MemoryStoreConcept<LongTermMemory>, "LongTermMemory must satisfy MemoryStoreConcept");
  SUCCEED();
}

// ── Dimension Validation (Consteval) ─────────────

// Example of a compile-time dimension validator
template<size_t Dim>
consteval bool validate_memory_dim() {
  static_assert(Dim > 0, "Memory dimension must be positive");
  static_assert(Dim <= 8192, "Dimension exceeds maximum reasonable limit (8192)");
  return true;
}

TEST(CompileSafetyTest, ConstevalValidation) {
  constexpr bool v1 = validate_memory_dim<1536>(); // OpenAI ada-002
  constexpr bool v2 = validate_memory_dim<768>();  // BERT base
  EXPECT_TRUE(v1);
  EXPECT_TRUE(v2);
}
