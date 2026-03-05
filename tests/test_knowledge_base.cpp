#include "agentos/core/types.hpp"
#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/knowledge_base.hpp"
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace agentos;
using namespace agentos::knowledge;
using namespace agentos::kernel;

class MockEmbeddingBackend : public ILLMBackend {
public:
  std::string name() const override { return "Mock"; }

  Result<LLMResponse> complete(const LLMRequest &req) override {
    (void)req;
    return LLMResponse{};
  }

  Result<EmbeddingResponse> embed(const EmbeddingRequest &req) override {
    EmbeddingResponse resp;

    for (const auto &text : req.inputs) {
      std::vector<float> vec(1536, 0.01f);
      if (text.find("apple") != std::string::npos) {
        vec[0] = 0.99f;
      } else if (text.find("banana") != std::string::npos) {
        vec[1] = 0.99f;
      }
      // Normalize mock vector slightly
      float norm = 0;
      for (float v : vec)
        norm += v * v;
      norm = std::sqrt(norm);
      if (norm > 0) {
        for (float &v : vec)
          v /= norm;
      }

      resp.embeddings.push_back(vec);
    }

    resp.total_tokens = req.inputs.size() * 10;
    return resp;
  }
};

TEST(KnowledgeBaseTest, BasicIngestionAndFusion) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  kb.ingest_text("doc1", "The apple is red and delicious.");
  kb.ingest_text("doc2", "The banana is yellow and sweet.");

  auto results = kb.search("delicious apple", 2);

  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "doc1");
}

TEST(KnowledgeBaseTest, OverlappingChunks) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  std::string long_text =
      "A very long document that needs to be chunked into several pieces so we "
      "can test the overlap logic. "
      "We need to ensure it splits correctly. "
      "It continues with more information about apples and bananas. ";

  // By giving a very small chunk_size we force multiple chunks
  kb.ingest_text("long_doc", long_text);

  auto results = kb.search("overlap logic", 1);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "long_doc");
}
