#include "agentos/core/types.hpp"
#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/knowledge_base.hpp"
#include "graph_engine/builder/graph_builder.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace agentos;
using namespace agentos::knowledge;
using namespace agentos::kernel;
namespace fs = std::filesystem;

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
      if (text.find("apple") != std::string::npos ||
          text.find("Apple") != std::string::npos) {
        vec[0] = 0.99f;
      } else if (text.find("banana") != std::string::npos) {
        vec[1] = 0.99f;
      }
      // Normalize
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

  kb.ingest_text("long_doc", long_text);

  auto results = kb.search("overlap logic", 1);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "long_doc");
}

TEST(KnowledgeBaseTest, GraphRAGIntegration) {
  // 1. 构建图
  fs::path graph_dir = "/tmp/agentos_kb_graph_test";
  if (fs::exists(graph_dir))
    fs::remove_all(graph_dir);

  {
    graph_engine::builder::GraphBuilder builder(graph_dir);
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    builder.add_edge("Apple", "iPhone", "product");
    builder.add_edge("Tim_Cook", "Apple", "manages");
    ASSERT_TRUE(builder.build());
  }

  auto graph =
      std::make_shared<graph_engine::core::ImmutableGraph>(graph_dir);
  ASSERT_TRUE(graph->load());

  // 2. 创建 KB 并 attach graph
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.attach_graph(graph);

  kb.ingest_text("news1",
                 "Apple announced a new product today. Tim Cook presented.");

  // 3. 搜索（带 GraphRAG 扩展）
  auto results = kb.search("Apple product", 2, /*graph_hops=*/1);
  ASSERT_FALSE(results.empty());

  // 应该有 graph_context 填充
  EXPECT_FALSE(results[0].graph_context.empty());

  // graph_context 应包含图关系
  EXPECT_NE(results[0].graph_context.find("Tim_Cook"), std::string::npos);
  EXPECT_NE(results[0].graph_context.find("iPhone"), std::string::npos);

  // 清理
  fs::remove_all(graph_dir);
}

TEST(KnowledgeBaseTest, SearchWithoutGraphReturnsEmptyContext) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  kb.ingest_text("doc1", "Some text about Apple products.");
  auto results = kb.search("Apple", 1);

  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results[0].graph_context.empty());
}

// ── KB 持久化测试 ──────────────────────────────
TEST(KnowledgeBaseTest, SaveAndLoad) {
  fs::path kb_dir = "/tmp/agentos_kb_persist_test";
  if (fs::exists(kb_dir))
    fs::remove_all(kb_dir);

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();

  // 1. 创建 KB 并 ingest 文档
  {
    KnowledgeBase kb(mock_llm, 1536);
    kb.ingest_text("doc1", "The apple is red and delicious.");
    kb.ingest_text("doc2", "The banana is yellow and sweet.");

    // 验证搜索正常
    auto results = kb.search("delicious apple", 2);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].doc_id, "doc1");

    // 保存
    ASSERT_TRUE(kb.save(kb_dir));
  }

  // 2. 从磁盘加载到新 KB
  {
    KnowledgeBase kb2(mock_llm, 1536);
    ASSERT_TRUE(kb2.load(kb_dir));

    // 验证 BM25 搜索正常（不依赖 HNSW）
    auto results = kb2.search("delicious apple", 2);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].doc_id, "doc1");

    // 验证内容完整
    auto banana_results = kb2.search("banana yellow", 1);
    ASSERT_FALSE(banana_results.empty());
    EXPECT_EQ(banana_results[0].doc_id, "doc2");
  }

  fs::remove_all(kb_dir);
}

TEST(KnowledgeBaseTest, LoadNonexistentReturnsFalse) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  EXPECT_FALSE(kb.load("/tmp/nonexistent_kb_dir_42"));
}
