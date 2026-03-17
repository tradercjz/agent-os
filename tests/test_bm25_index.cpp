#include "agentos/knowledge/bm25_index.hpp"
#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <string>

using namespace agentos::knowledge;
namespace fs = std::filesystem;

class BM25IndexTest : public ::testing::Test {
protected:
  void SetUp() override {
    // Optional: Any setup code if needed.
  }

  void TearDown() override {
    // Optional: Clean up code if needed.
  }
};

TEST_F(BM25IndexTest, BasicFunctionality) {
  BM25Index index;

  index.add_document("doc1", "apple is delicious");
  index.add_document("doc2", "banana is sweet");
  index.add_document("doc3", "apple and banana");

  EXPECT_EQ(index.size(), 3u);

  auto results = index.search("apple", 10);
  ASSERT_FALSE(results.empty());

  // doc1 or doc3 should be the first
  bool found_doc1 = false;
  bool found_doc3 = false;
  for (const auto& match : results) {
    if (match.doc_id == "doc1") found_doc1 = true;
    if (match.doc_id == "doc3") found_doc3 = true;
  }
  EXPECT_TRUE(found_doc1);
  EXPECT_TRUE(found_doc3);

  // doc2 shouldn't be here since it has no apple
  for (const auto& match : results) {
    EXPECT_NE(match.doc_id, "doc2");
  }
}

TEST_F(BM25IndexTest, SearchNonExistentTerm) {
  BM25Index index;
  index.add_document("doc1", "apple is delicious");

  auto results = index.search("orange", 10);
  EXPECT_TRUE(results.empty());
}

TEST_F(BM25IndexTest, RemoveDocument) {
  BM25Index index;
  index.add_document("doc1", "apple is delicious");
  index.add_document("doc2", "banana is sweet");

  EXPECT_EQ(index.size(), 2u);

  EXPECT_TRUE(index.remove_document("doc1"));
  EXPECT_EQ(index.size(), 1u);

  auto results = index.search("apple", 10);
  EXPECT_TRUE(results.empty());

  results = index.search("banana", 10);
  ASSERT_FALSE(results.empty());
  EXPECT_EQ(results[0].doc_id, "doc2");
}

TEST_F(BM25IndexTest, RemoveNonExistentDocument) {
  BM25Index index;
  index.add_document("doc1", "apple is delicious");

  EXPECT_FALSE(index.remove_document("doc2"));
  EXPECT_EQ(index.size(), 1u);
}

TEST_F(BM25IndexTest, EmptyDocumentAndIndex) {
  BM25Index index;

  // Empty index search
  auto results = index.search("apple", 10);
  EXPECT_TRUE(results.empty());

  // Add empty document
  index.add_document("doc1", "");
  EXPECT_EQ(index.size(), 0u);
}

TEST_F(BM25IndexTest, SaveAndLoad) {
  fs::path persist_file = fs::temp_directory_path() / "agentos_bm25_test.bin";
  if (fs::exists(persist_file)) {
    fs::remove(persist_file);
  }

  {
    BM25Index index;
    index.add_document("doc1", "apple is red");
    index.add_document("doc2", "banana is yellow");

    EXPECT_TRUE(index.save(persist_file));
  }

  {
    BM25Index index;
    EXPECT_TRUE(index.load(persist_file));

    EXPECT_EQ(index.size(), 2u);

    auto results = index.search("apple", 10);
    ASSERT_FALSE(results.empty());
    EXPECT_EQ(results[0].doc_id, "doc1");
  }

  if (fs::exists(persist_file)) {
    fs::remove(persist_file);
  }
}

TEST_F(BM25IndexTest, LoadNonExistentFile) {
  BM25Index index;
  EXPECT_FALSE(index.load(fs::temp_directory_path() / "nonexistent_bm25_file.bin"));
}
