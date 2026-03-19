#include "agentos/knowledge/tokenizer.hpp"
#include <gtest/gtest.h>
#include <string>
#include <vector>

using namespace agentos::knowledge;

TEST(TokenizerTest, IsReady) {
  auto& tokenizer = Tokenizer::instance();
  EXPECT_TRUE(tokenizer.is_ready());
}

TEST(TokenizerTest, EmptyString) {
  auto& tokenizer = Tokenizer::instance();
  std::vector<std::string> tokens = tokenizer.cut("");
  EXPECT_TRUE(tokens.empty());
}

TEST(TokenizerTest, BasicEnglishTokenization) {
  auto& tokenizer = Tokenizer::instance();
  std::string text = "Hello, world! This is a test.";
  std::vector<std::string> tokens = tokenizer.cut(text);

  // The exact tokenization depends on whether JIEBA is enabled,
  // but it should definitely split out basic words.
  EXPECT_FALSE(tokens.empty());

  // We can just verify it has some of the expected words
  bool found_hello = false;
  bool found_world = false;
  bool found_test = false;

  for (const auto& token : tokens) {
    if (token == "Hello" || token == "hello") found_hello = true;
    if (token == "world" || token == "World") found_world = true;
    if (token == "test" || token == "Test") found_test = true;
  }

  EXPECT_TRUE(found_hello);
  EXPECT_TRUE(found_world);
  EXPECT_TRUE(found_test);
}

#ifndef AGENTOS_ENABLE_JIEBA
TEST(TokenizerTest, FallbackWhitespaceAndPunctuation) {
  auto& tokenizer = Tokenizer::instance();
  std::string text = "a,b.c!d?e;f:g h\ti\nj\rk";
  std::vector<std::string> tokens = tokenizer.cut(text);

  // Should have exactly 11 tokens: a, b, c, d, e, f, g, h, i, j, k
  EXPECT_EQ(tokens.size(), 11u);
  EXPECT_EQ(tokens[0], "a");
  EXPECT_EQ(tokens[1], "b");
  EXPECT_EQ(tokens[2], "c");
  EXPECT_EQ(tokens[3], "d");
  EXPECT_EQ(tokens[4], "e");
  EXPECT_EQ(tokens[5], "f");
  EXPECT_EQ(tokens[6], "g");
  EXPECT_EQ(tokens[7], "h");
  EXPECT_EQ(tokens[8], "i");
  EXPECT_EQ(tokens[9], "j");
  EXPECT_EQ(tokens[10], "k");
}
#endif
