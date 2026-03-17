#include <gtest/gtest.h>
#include "agentos/knowledge/document.hpp"

using namespace agentos::knowledge;

TEST(DocumentTest, ChunkInstantiationAndFieldAccess) {
  Chunk chunk;
  chunk.id = "doc1_chunk1";
  chunk.document_id = "doc1";
  chunk.content = "This is a test chunk.";
  chunk.metadata["start_line"] = "1";
  chunk.metadata["end_line"] = "10";

  EXPECT_EQ(chunk.id, "doc1_chunk1");
  EXPECT_EQ(chunk.document_id, "doc1");
  EXPECT_EQ(chunk.content, "This is a test chunk.");
  EXPECT_EQ(chunk.metadata["start_line"], "1");
  EXPECT_EQ(chunk.metadata["end_line"], "10");
}

TEST(DocumentTest, DocumentInstantiationAndFieldAccess) {
  Document doc;
  doc.id = "/path/to/doc.txt";
  doc.title = "Test Document";
  doc.content = "This is a test document full content.";
  doc.content_hash = 123456789;

  Chunk chunk1{"doc_c1", "/path/to/doc.txt", "This is", {{"line", "1"}}};
  Chunk chunk2{"doc_c2", "/path/to/doc.txt", " a test", {{"line", "2"}}};
  doc.chunks = {chunk1, chunk2};

  doc.metadata["author"] = "Jules";
  doc.metadata["created_at"] = "2023-10-27";

  EXPECT_EQ(doc.id, "/path/to/doc.txt");
  EXPECT_EQ(doc.title, "Test Document");
  EXPECT_EQ(doc.content, "This is a test document full content.");
  EXPECT_EQ(doc.content_hash, 123456789);

  ASSERT_EQ(doc.chunks.size(), 2);
  EXPECT_EQ(doc.chunks[0].id, "doc_c1");
  EXPECT_EQ(doc.chunks[1].content, " a test");

  EXPECT_EQ(doc.metadata["author"], "Jules");
}

TEST(DocumentTest, DocumentComputeHash) {
  Document doc1;
  doc1.content = "Same content";
  doc1.compute_hash();

  Document doc2;
  doc2.content = "Same content";
  doc2.compute_hash();

  Document doc3;
  doc3.content = "Different content";
  doc3.compute_hash();

  // Consistent hashing for same content
  EXPECT_EQ(doc1.content_hash, doc2.content_hash);

  // Different hash for different content
  EXPECT_NE(doc1.content_hash, doc3.content_hash);
}
