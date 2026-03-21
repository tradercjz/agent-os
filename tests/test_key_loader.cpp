#include <agentos/core/key_loader.hpp>
#include <gtest/gtest.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace agentos;

TEST(KeyLoaderTest, ExplicitValueTakesPriority) {
    auto r = KeyLoader::load("explicit-key", "NONEXISTENT_ENV_VAR", "test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "explicit-key");
}

TEST(KeyLoaderTest, FallsToEnvVar) {
    setenv("AGENTOS_TEST_KEY", "env-key-123", 1);
    auto r = KeyLoader::load("", "AGENTOS_TEST_KEY", "test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "env-key-123");
    unsetenv("AGENTOS_TEST_KEY");
}

TEST(KeyLoaderTest, FromFileParsesJson) {
    auto tmp = std::filesystem::temp_directory_path() / "test_keys.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({"openai": "sk-test-123", "anthropic": "sk-ant-456"})";
    }
    auto r = KeyLoader::from_file("openai", tmp.string());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "sk-test-123");
    std::filesystem::remove(tmp);
}

TEST(KeyLoaderTest, FromFileSecondKey) {
    auto tmp = std::filesystem::temp_directory_path() / "test_keys2.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({"openai": "sk-test-123", "anthropic": "sk-ant-456"})";
    }
    auto r = KeyLoader::from_file("anthropic", tmp.string());
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "sk-ant-456");
    std::filesystem::remove(tmp);
}

TEST(KeyLoaderTest, MissingKeyReturnsError) {
    auto r = KeyLoader::load("", "DEFINITELY_NOT_SET_12345", "nonexistent_key");
    EXPECT_FALSE(r.has_value());
}

TEST(KeyLoaderTest, MissingFileSkipsSilently) {
    auto r = KeyLoader::from_file("test", "/nonexistent/path/keys.json");
    EXPECT_FALSE(r.has_value());
}

TEST(KeyLoaderTest, EmptyKeyInFileReturnsError) {
    auto tmp = std::filesystem::temp_directory_path() / "test_empty_key.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({"openai": "", "anthropic": "sk-ant-456"})";
    }
    auto r = KeyLoader::from_file("openai", tmp.string());
    EXPECT_FALSE(r.has_value());
    std::filesystem::remove(tmp);
}

TEST(KeyLoaderTest, MalformedJsonReturnsError) {
    auto tmp = std::filesystem::temp_directory_path() / "test_bad_keys.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({invalid json})";
    }
    auto r = KeyLoader::from_file("openai", tmp.string());
    EXPECT_FALSE(r.has_value());
    std::filesystem::remove(tmp);
}

TEST(KeyLoaderTest, DefaultKeyFileContainsAgentos) {
    auto path = KeyLoader::default_key_file();
    // Should contain .agentos/keys.json if HOME is set
    if (!path.empty()) {
        EXPECT_NE(path.find(".agentos/keys.json"), std::string::npos);
    }
}

TEST(KeyLoaderTest, FallbackChainEnvBeforeFile) {
    // Set env var — should use it even if file would work
    setenv("AGENTOS_CHAIN_TEST", "from-env", 1);
    auto tmp = std::filesystem::temp_directory_path() / "test_chain_keys.json";
    {
        std::ofstream ofs(tmp);
        ofs << R"({"chain_key": "from-file"})";
    }
    auto r = KeyLoader::load("", "AGENTOS_CHAIN_TEST", "chain_key");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "from-env"); // env takes priority over file
    unsetenv("AGENTOS_CHAIN_TEST");
    std::filesystem::remove(tmp);
}
