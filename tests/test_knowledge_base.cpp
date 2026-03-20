#include "agentos/core/types.hpp"
#include "agentos/kernel/llm_kernel.hpp"
#include "agentos/knowledge/knowledge_base.hpp"
#include "graph_engine/builder/graph_builder.hpp"
#include "graph_engine/core/immutable_graph.hpp"
#include <chrono>
#include <filesystem>
#include <gtest/gtest.h>
#include <memory>
#include <string>
#include <vector>

using namespace agentos;
using namespace agentos::knowledge;
using namespace agentos::kernel;
namespace fs = std::filesystem;

namespace {

fs::path make_kb_test_path(const std::string &name) {
  const auto nonce = std::chrono::steady_clock::now().time_since_epoch().count();
  return fs::temp_directory_path() /
         ("agentos_kb_test_" + name + "_" + std::to_string(nonce));
}

}

class MockEmbeddingBackend : public ILLMBackend {
public:
  std::string name() const noexcept override { return "Mock"; }

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

  (void)kb.ingest_text("doc1", "The apple is red and delicious.");
  (void)kb.ingest_text("doc2", "The banana is yellow and sweet.");

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

  (void)kb.ingest_text("long_doc", long_text);

  auto results = kb.search("overlap logic", 1);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "long_doc");
}

TEST(KnowledgeBaseTest, GraphRAGIntegration) {
  // 1. 构建图
  fs::path graph_dir = make_kb_test_path("graph");
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

  (void)kb.ingest_text("news1",
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

  (void)kb.ingest_text("doc1", "Some text about Apple products.");
  auto results = kb.search("Apple", 1);

  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results[0].graph_context.empty());
}

// ── KB 持久化测试 ──────────────────────────────
TEST(KnowledgeBaseTest, SaveAndLoad) {
  fs::path kb_dir = make_kb_test_path("persist");
  if (fs::exists(kb_dir))
    fs::remove_all(kb_dir);

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();

  // 1. 创建 KB 并 ingest 文档
  {
    KnowledgeBase kb(mock_llm, 1536);
    (void)kb.ingest_text("doc1", "The apple is red and delicious.");
    (void)kb.ingest_text("doc2", "The banana is yellow and sweet.");

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
  EXPECT_FALSE(kb.load(make_kb_test_path("missing_load")));
}

// ── 文档删除测试 ──────────────────────────────
// ── BM25 文档删除测试 ──────────────────────────────
TEST(KnowledgeBaseTest, BM25RemoveDocumentCleansIndex) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("doc_apple", "Apple is a delicious red fruit.");
  (void)kb.ingest_text("doc_banana", "Banana is a yellow tropical fruit.");

  // 删除前：搜索 "apple" 应返回 doc_apple
  auto before = kb.search("apple fruit", 5);
  bool found_apple = false;
  for (const auto &r : before) {
    if (r.doc_id == "doc_apple")
      found_apple = true;
  }
  EXPECT_TRUE(found_apple);

  // 删除 doc_apple
  EXPECT_TRUE(kb.remove_document("doc_apple"));
  EXPECT_EQ(kb.document_count(), 1u);

  // 删除后：BM25 不应再返回已删除文档的结果
  auto after = kb.search("apple fruit", 5);
  for (const auto &r : after) {
    EXPECT_NE(r.doc_id, "doc_apple");
  }

  // banana 仍可搜到
  auto banana_results = kb.search("banana tropical", 5);
  bool found_banana = false;
  for (const auto &r : banana_results) {
    if (r.doc_id == "doc_banana")
      found_banana = true;
  }
  EXPECT_TRUE(found_banana);
}

TEST(KnowledgeBaseTest, RemoveDocumentRemovesChunks) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("doc_to_remove", "Content about Apple products.");
  (void)kb.ingest_text("doc_keep", "Content about banana smoothies.");

  EXPECT_EQ(kb.document_count(), 2u);
  EXPECT_GE(kb.chunk_count(), 2u);

  // 删除 doc_to_remove
  EXPECT_TRUE(kb.remove_document("doc_to_remove"));

  // 搜索应不再返回已删除文档
  auto results = kb.search("Apple products", 5);
  for (const auto &r : results) {
    EXPECT_NE(r.doc_id, "doc_to_remove");
  }

  // 保留的文档仍可搜到
  auto banana_results = kb.search("banana smoothies", 1);
  ASSERT_FALSE(banana_results.empty());
  EXPECT_EQ(banana_results[0].doc_id, "doc_keep");
}

TEST(KnowledgeBaseTest, RemoveNonexistentDocReturnsFalse) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  EXPECT_FALSE(kb.remove_document("nonexistent_doc"));
}

// ── 可配置 embedding 模型测试 ──────────────────
TEST(KnowledgeBaseTest, EmbeddingModelConfigurable) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536, 100000, "custom-model-v1");

  EXPECT_EQ(kb.embedding_model(), "custom-model-v1");

  kb.set_embedding_model("custom-model-v2");
  EXPECT_EQ(kb.embedding_model(), "custom-model-v2");
}

// ── 可配置分块参数测试 ──────────────────────────
TEST(KnowledgeBaseTest, ChunkConfigurable) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  // 默认值
  EXPECT_EQ(kb.chunk_size(), 500u);
  EXPECT_EQ(kb.chunk_overlap(), 50u);

  // 修改
  kb.set_chunk_params(200, 30);
  EXPECT_EQ(kb.chunk_size(), 200u);
  EXPECT_EQ(kb.chunk_overlap(), 30u);

  // 小 chunk 会产生更多分块
  std::string long_text =
      "This is a sentence about apples. Another sentence about bananas. "
      "More text about oranges. Some content about grapes. Final words about "
      "pears and melons for testing the chunking mechanism.";
  (void)kb.ingest_text("chunked_doc", long_text);
  EXPECT_GE(kb.chunk_count(), 1u);
}

// ── 统计方法测试 ──────────────────────────────
TEST(KnowledgeBaseTest, DocumentAndChunkCounts) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  EXPECT_EQ(kb.document_count(), 0u);
  EXPECT_EQ(kb.chunk_count(), 0u);

  (void)kb.ingest_text("d1", "First document.");
  (void)kb.ingest_text("d2", "Second document.");

  EXPECT_EQ(kb.document_count(), 2u);
  EXPECT_GE(kb.chunk_count(), 2u);
}

// ── Coverage boost tests ────────────────────────────────

// Test chunking with empty text
TEST(KnowledgeBaseTest, IngestEmptyText) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  auto result = kb.ingest_text("empty_doc", "");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(*result, 0u);
  EXPECT_EQ(kb.document_count(), 0u);
  EXPECT_EQ(kb.chunk_count(), 0u);
}

// Test chunk deduplication: re-ingesting same doc_id skips duplicates
TEST(KnowledgeBaseTest, IngestDeduplication) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("dup_doc", "Apple is great.");
  auto count_before = kb.chunk_count();
  // Re-ingest same doc_id — chunks should be skipped
  (void)kb.ingest_text("dup_doc", "Apple is great.");
  EXPECT_EQ(kb.chunk_count(), count_before);
}

// Test search with empty knowledge base
TEST(KnowledgeBaseTest, SearchEmptyKB) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  auto results = kb.search("anything", 5);
  EXPECT_TRUE(results.empty());
}

// Test search returns top_k results when more are available
TEST(KnowledgeBaseTest, SearchRespectsTopK) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("d1", "Apple product one.");
  (void)kb.ingest_text("d2", "Apple product two.");
  (void)kb.ingest_text("d3", "Apple product three.");

  auto results = kb.search("apple", 1);
  EXPECT_LE(results.size(), 1u);

  auto results2 = kb.search("apple", 10);
  EXPECT_GE(results2.size(), 1u);
}

// Test ingest_text with very long text triggers chunking overlap
TEST(KnowledgeBaseTest, LongTextChunkingWithOverlap) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.set_chunk_params(50, 10);

  // Generate a long multi-sentence text
  std::string long_text;
  for (int i = 0; i < 20; ++i) {
    long_text += "Sentence number " + std::to_string(i) + " about apple products. ";
  }

  auto result = kb.ingest_text("long_doc", long_text);
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(*result, 1u); // Should produce multiple chunks
  EXPECT_GT(kb.chunk_count(), 1u);
}

// Test sentence splitting with Chinese/CJK punctuation
TEST(KnowledgeBaseTest, SentenceSplitCJK) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.set_chunk_params(30, 5);

  // Use CJK sentence terminators (。！？)
  std::string cjk_text = "Apple is great\xe3\x80\x82"
                          "Banana is good\xef\xbc\x81"
                          "Cherry is nice\xef\xbc\x9f"
                          "Date is sweet\xef\xbc\x9b";

  auto result = kb.ingest_text("cjk_doc", cjk_text);
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(*result, 1u);
}

// Test ingest_directory with nonexistent directory
TEST(KnowledgeBaseTest, IngestDirectoryNonexistent) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  // Should not crash, just log error
  kb.ingest_directory(make_kb_test_path("missing_ingest"));
  EXPECT_EQ(kb.document_count(), 0u);
}

// Test ingest_directory with actual files
TEST(KnowledgeBaseTest, IngestDirectoryWithFiles) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  fs::path test_dir = make_kb_test_path("ingest_dir");
  fs::remove_all(test_dir);
  fs::create_directories(test_dir);

  // Create .txt and .md files
  {
    std::ofstream(test_dir / "a.txt") << "Apple is a great fruit.";
    std::ofstream(test_dir / "b.md") << "Banana is yellow.";
    std::ofstream(test_dir / "c.cpp") << "This should be ignored.";
  }

  kb.ingest_directory(test_dir);
  EXPECT_GE(kb.document_count(), 2u);

  fs::remove_all(test_dir);
}

// Test save/load roundtrip preserves embedding model name
TEST(KnowledgeBaseTest, SaveLoadPreservesEmbeddingModel) {
  fs::path kb_dir = make_kb_test_path("model");
  fs::remove_all(kb_dir);

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();

  {
    KnowledgeBase kb(mock_llm, 1536, 100000, "custom-embed-v3");
    (void)kb.ingest_text("d1", "Content about apple.");
    ASSERT_TRUE(kb.save(kb_dir));
  }
  {
    KnowledgeBase kb2(mock_llm, 1536);
    ASSERT_TRUE(kb2.load(kb_dir));
    // DuckDB persists the embedding model name
    EXPECT_EQ(kb2.embedding_model(), "custom-embed-v3");
  }

  fs::remove_all(kb_dir);
}

// Test search result includes correct content and doc_id fields
TEST(KnowledgeBaseTest, SearchResultFields) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("doc_apple", "Apple is delicious and red.");
  auto results = kb.search("apple delicious", 1);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "doc_apple");
  EXPECT_FALSE(results[0].content.empty());
  EXPECT_GT(results[0].score, 0.0);
  EXPECT_TRUE(results[0].graph_context.empty()); // no graph attached
}

// Test remove all documents leaves KB empty
TEST(KnowledgeBaseTest, RemoveAllDocuments) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("d1", "Apple content.");
  (void)kb.ingest_text("d2", "Banana content.");

  EXPECT_TRUE(kb.remove_document("d1"));
  EXPECT_TRUE(kb.remove_document("d2"));
  EXPECT_EQ(kb.document_count(), 0u);
  EXPECT_EQ(kb.chunk_count(), 0u);

  auto results = kb.search("apple", 5);
  EXPECT_TRUE(results.empty());
}

// Test search with graph_hops=0 does not add graph context even if graph attached
TEST(KnowledgeBaseTest, SearchWithGraphHopsZero) {
  fs::path graph_dir = make_kb_test_path("graph_hops0");
  fs::remove_all(graph_dir);

  {
    graph_engine::builder::GraphBuilder builder(graph_dir);
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    ASSERT_TRUE(builder.build());
  }

  auto graph =
      std::make_shared<graph_engine::core::ImmutableGraph>(graph_dir);
  ASSERT_TRUE(graph->load());

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.attach_graph(graph);

  (void)kb.ingest_text("n1", "Apple announced new iPhone today.");
  auto results = kb.search("Apple iPhone", 2, /*graph_hops=*/0);
  ASSERT_FALSE(results.empty());
  EXPECT_TRUE(results[0].graph_context.empty()); // hops=0 means no graph expansion

  fs::remove_all(graph_dir);
}

// Test single very long sentence that exceeds chunk_size (super-long sentence edge case)
TEST(KnowledgeBaseTest, SingleLongSentenceChunk) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.set_chunk_params(20, 5);

  // One sentence with no sentence boundaries, longer than chunk_size
  std::string long_sentence(200, 'a');
  auto result = kb.ingest_text("long_sent", long_sentence);
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(*result, 1u);
}

// ── Coverage boost: legacy binary format load ──────────────────
TEST(KnowledgeBaseTest, LoadFromLegacyBin) {
  fs::path kb_dir = make_kb_test_path("legacy");
  fs::remove_all(kb_dir);
  fs::create_directories(kb_dir);

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();

  // Step 1: Save using DuckDB format, then also create a legacy binary
  {
    KnowledgeBase kb(mock_llm, 1536);
    (void)kb.ingest_text("legacy_doc", "Apple is a great company.");
    ASSERT_TRUE(kb.save(kb_dir));
  }

  // Step 2: Remove the DuckDB file but keep BM25 — then create a legacy .bin
  fs::remove(kb_dir / "kb_meta.duckdb");
  fs::remove(kb_dir / "kb_meta.duckdb.wal");
  {
    // Write a valid legacy binary file
    std::ofstream ofs(kb_dir / "kb_meta.bin", std::ios::binary);
    uint32_t magic = 0x4B424D54;
    ofs.write(reinterpret_cast<const char *>(&magic), 4);

    uint32_t dim = 1536;
    ofs.write(reinterpret_cast<const char *>(&dim), sizeof(dim));

    size_t max_chunks = 100000;
    ofs.write(reinterpret_cast<const char *>(&max_chunks), sizeof(max_chunks));

    // embedding_model string
    auto write_str = [&](const std::string &s) {
      uint32_t len = static_cast<uint32_t>(s.size());
      ofs.write(reinterpret_cast<const char *>(&len), 4);
      ofs.write(s.data(), len);
    };
    write_str("text-embedding-3-small");

    // n_chunks = 1
    uint32_t n_chunks = 1;
    ofs.write(reinterpret_cast<const char *>(&n_chunks), 4);
    write_str("legacy_doc_chunk_0");
    write_str("Apple is a great company.");

    // n_docs = 1
    uint32_t n_docs = 1;
    ofs.write(reinterpret_cast<const char *>(&n_docs), 4);
    write_str("legacy_doc_chunk_0");
    write_str("legacy_doc");

    // next_hnsw_id
    hnswlib::labeltype next_id = 0;
    ofs.write(reinterpret_cast<const char *>(&next_id), sizeof(next_id));

    // n_hnsw = 0
    uint32_t n_hnsw = 0;
    ofs.write(reinterpret_cast<const char *>(&n_hnsw), 4);
  }

  // Step 3: Load from legacy format
  {
    KnowledgeBase kb2(mock_llm, 1536);
    ASSERT_TRUE(kb2.load(kb_dir));
    EXPECT_GE(kb2.chunk_count(), 1u);
    EXPECT_GE(kb2.document_count(), 1u);
  }

  fs::remove_all(kb_dir);
}

// Test loading legacy bin with bad magic returns false
TEST(KnowledgeBaseTest, LoadLegacyBinBadMagic) {
  fs::path kb_dir = make_kb_test_path("legacy_bad_magic");
  fs::remove_all(kb_dir);
  fs::create_directories(kb_dir);

  // Write BM25 index (empty)
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  {
    KnowledgeBase kb(mock_llm, 1536);
    ASSERT_TRUE(kb.save(kb_dir));
  }
  // Remove DuckDB, write bad magic legacy bin
  fs::remove(kb_dir / "kb_meta.duckdb");
  fs::remove(kb_dir / "kb_meta.duckdb.wal");
  {
    std::ofstream ofs(kb_dir / "kb_meta.bin", std::ios::binary);
    uint32_t bad_magic = 0xDEADBEEF;
    ofs.write(reinterpret_cast<const char *>(&bad_magic), 4);
  }

  {
    KnowledgeBase kb2(mock_llm, 1536);
    EXPECT_FALSE(kb2.load(kb_dir));
  }

  fs::remove_all(kb_dir);
}

// Test UTF-8 multi-byte character handling in split_sentences
TEST(KnowledgeBaseTest, UTF8MultiByteSplitting) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.set_chunk_params(30, 5);

  // 4-byte UTF-8 characters (emoji: U+1F600 = \xF0\x9F\x98\x80)
  std::string text_with_emoji = "Apple is great\xF0\x9F\x98\x80. Banana is good. Cherry!";
  auto result = kb.ingest_text("emoji_doc", text_with_emoji);
  ASSERT_TRUE(result.has_value());
  EXPECT_GE(*result, 1u);

  // 2-byte UTF-8 characters (e.g., U+00E9 = \xC3\xA9 = e-acute)
  std::string text_with_accents = "Caf\xC3\xA9 is nice. R\xC3\xA9sum\xC3\xA9 is ready.";
  auto result2 = kb.ingest_text("accent_doc", text_with_accents);
  ASSERT_TRUE(result2.has_value());
  EXPECT_GE(*result2, 1u);

  // Truncated UTF-8 sequence at end of string (clen=1 fallback)
  std::string truncated = "Apple test\xC3"; // 2-byte start but only 1 byte
  auto result3 = kb.ingest_text("trunc_doc", truncated);
  ASSERT_TRUE(result3.has_value());
}

// Test RRF fusion skips deleted chunks
TEST(KnowledgeBaseTest, RRFFusionSkipsDeletedChunks) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("doc_a", "Apple product announcement today.");
  (void)kb.ingest_text("doc_b", "Banana smoothie recipe.");

  // Remove doc_a, then search — RRF should skip deleted chunks from BM25
  EXPECT_TRUE(kb.remove_document("doc_a"));

  auto results = kb.search("Apple product", 5);
  for (const auto &r : results) {
    EXPECT_NE(r.doc_id, "doc_a");
  }
}

// Test GraphRAG with entity not found in graph (empty triples)
TEST(KnowledgeBaseTest, GraphRAGEntityNotInGraph) {
  fs::path graph_dir = make_kb_test_path("graph_notfound");
  fs::remove_all(graph_dir);

  {
    graph_engine::builder::GraphBuilder builder(graph_dir);
    builder.add_edge("Apple", "Tim_Cook", "ceo_of");
    ASSERT_TRUE(builder.build());
  }

  auto graph =
      std::make_shared<graph_engine::core::ImmutableGraph>(graph_dir);
  ASSERT_TRUE(graph->load());

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);
  kb.attach_graph(graph);

  // Ingest text with entity "Google" which is NOT in the graph
  (void)kb.ingest_text("n1", "Google launched a new product today.");

  auto results = kb.search("Google product", 2, /*graph_hops=*/1);
  // Should still get results, but graph_context may be empty (entity not in graph)
  // This exercises the "matched_entities.empty()" and "sub.triples.empty()" paths
  ASSERT_FALSE(results.empty());

  fs::remove_all(graph_dir);
}

// Test GraphRAG with no graph attached but hops > 0 (should be no-op)
TEST(KnowledgeBaseTest, GraphRAGNoGraphAttached) {
  auto mock_llm = std::make_shared<MockEmbeddingBackend>();
  KnowledgeBase kb(mock_llm, 1536);

  (void)kb.ingest_text("n1", "Apple product launch today.");
  auto results = kb.search("Apple", 2, /*graph_hops=*/2);
  ASSERT_FALSE(results.empty());
  // No graph attached, so graph_context should be empty
  EXPECT_TRUE(results[0].graph_context.empty());
}

// Test search with HNSW unavailable (BM25-only fallback)
// This is tested implicitly since MockEmbeddingBackend always returns embeddings,
// but we can test by searching after save/load without HNSW file
TEST(KnowledgeBaseTest, SearchBM25FallbackNoHNSW) {
  fs::path kb_dir = make_kb_test_path("bm25only");
  fs::remove_all(kb_dir);

  auto mock_llm = std::make_shared<MockEmbeddingBackend>();

  {
    KnowledgeBase kb(mock_llm, 1536);
    (void)kb.ingest_text("d1", "Apple is a technology company.");
    (void)kb.ingest_text("d2", "Banana is a tropical fruit.");
    ASSERT_TRUE(kb.save(kb_dir));
  }

  // Remove HNSW file so load will use BM25-only
  fs::remove(kb_dir / "hnsw_index.bin");

  {
    KnowledgeBase kb2(mock_llm, 1536);
    ASSERT_TRUE(kb2.load(kb_dir));
    auto results = kb2.search("Apple technology", 2);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].doc_id, "d1");
  }

  fs::remove_all(kb_dir);
}
