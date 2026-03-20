#include <gtest/gtest.h>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

// Test the exception handling pattern used in the DolphinDB plugin.
// Since the actual macros now depend on DolphinDB SDK headers (Exceptions.h),
// we replicate the pattern here with mock exception types for testing.

class IllegalArgumentException : public std::exception {
public:
    IllegalArgumentException(const std::string&, const std::string&) {}
    const char* what() const noexcept override { return "IllegalArgumentException"; }
};

class RuntimeException : public std::exception {
public:
    RuntimeException(const std::string& errMsg) : msg_(errMsg) {}
    const char* what() const noexcept override { return msg_.c_str(); }
private:
    std::string msg_;
};

// Replicate the macro pattern from dolphindb_macros.hpp
#define TEST_DDB_SAFE_BEGIN try {
#define TEST_DDB_SAFE_END(func_name) \
    } catch (const IllegalArgumentException&) { throw; } \
      catch (const RuntimeException&) { throw; } \
      catch (const std::exception& e) { \
          throw RuntimeException(std::string(func_name " error: ") + e.what()); \
      } catch (...) { \
          throw RuntimeException(func_name ": unknown internal error"); \
      }

void testFunction(int type) {
    TEST_DDB_SAFE_BEGIN
        if (type == 1) throw IllegalArgumentException("test", "arg error");
        if (type == 2) throw RuntimeException("runtime error");
        if (type == 3) throw std::runtime_error("std error");
        if (type == 4) throw 42;
    TEST_DDB_SAFE_END("testFunction")
}

TEST(DolphinDBAdapterTest, ExceptionHandling) {
    // 1. IllegalArgumentException should be rethrown as is
    EXPECT_THROW(testFunction(1), IllegalArgumentException);

    // 2. RuntimeException should be rethrown as is
    EXPECT_THROW(testFunction(2), RuntimeException);

    // 3. std::exception should be caught and thrown as RuntimeException
    try {
        testFunction(3);
        FAIL() << "Expected RuntimeException";
    } catch (const RuntimeException& e) {
        EXPECT_EQ(std::string(e.what()), "testFunction error: std error");
    }

    // 4. Unknown exception should be caught and thrown as RuntimeException
    try {
        testFunction(4);
        FAIL() << "Expected RuntimeException";
    } catch (const RuntimeException& e) {
        EXPECT_EQ(std::string(e.what()), "testFunction: unknown internal error");
    }
}

TEST(DolphinDBAdapterTest, NoExceptionPassesThrough) {
    EXPECT_NO_THROW(testFunction(0));
}

static std::string read_file(const std::filesystem::path& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return std::string((std::istreambuf_iterator<char>(ifs)),
                       std::istreambuf_iterator<char>());
}

TEST(DolphinDBAdapterTest, V2AliasExportsAndDeclarationsPresent) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string manifest =
        read_file(repo_root / "plugins" / "dolphindb" / "PluginAgentOS.txt");
    EXPECT_NE(manifest.find("agentOSCreateAgent2,createAgent2"), std::string::npos);
    EXPECT_NE(manifest.find("agentOSAsk2,ask2"), std::string::npos);
    EXPECT_NE(manifest.find("agentOSAskStream2,askStream2"), std::string::npos);

    const std::string header =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.hpp");
    EXPECT_NE(header.find("ConstantSP agentOSCreateAgent2("), std::string::npos);
    EXPECT_NE(header.find("ConstantSP agentOSAsk2("), std::string::npos);
    EXPECT_NE(header.find("ConstantSP agentOSAskStream2("), std::string::npos);

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");
    EXPECT_NE(source.find("ConstantSP agentOSCreateAgent2("), std::string::npos);
    EXPECT_NE(source.find("ConstantSP agentOSAsk2("), std::string::npos);
    EXPECT_NE(source.find("ConstantSP agentOSAskStream2("), std::string::npos);
}

TEST(DolphinDBAdapterTest, V2AliasUsageStringsAreExplicit) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");

    EXPECT_NE(source.find("Usage: agentOS::createAgent2(name, [prompt], [tools], [skills], "
                          "[blockTools], [contextLimit], [isolation], [securityRole])"),
              std::string::npos);
    EXPECT_NE(source.find("Usage: agentOS::ask2(agent, question, [prompt])"),
              std::string::npos);
    EXPECT_NE(source.find("Usage: agentOS::askStream2(agent, question, [prompt], [callback])"),
              std::string::npos);
}

TEST(DolphinDBAdapterTest, V2HeaderCommentsRecommendExplicitAliases) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string header =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.hpp");

    EXPECT_NE(header.find("兼容入口：支持原生参数风格创建持久 Agent"), std::string::npos);
    EXPECT_NE(header.find("推荐写法：显式 V2 创建持久 Agent"), std::string::npos);
    EXPECT_NE(header.find("兼容入口：支持单轮 ask(question, ...) 和持久 Agent ask(agent, ...)"), std::string::npos);
    EXPECT_NE(header.find("推荐写法：要求第一个参数为 agent handle"), std::string::npos);
    EXPECT_NE(header.find("兼容入口：支持单轮 askStream(question, ...) 和持久 Agent askStream(agent, ...)"), std::string::npos);
}

TEST(DolphinDBAdapterTest, CompatibilityUsageStringsPointToV2Aliases) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");

    EXPECT_NE(source.find("For persistent agents, prefer agentOS::createAgent2(...)"),
              std::string::npos);
    EXPECT_NE(source.find("For persistent agents, prefer agentOS::ask2(agent, question, [prompt])"),
              std::string::npos);
    EXPECT_NE(source.find("For persistent agents, prefer agentOS::askStream2(agent, question, [prompt], [callback])"),
              std::string::npos);
}

TEST(DolphinDBAdapterTest, LegacyUsageStringsPointToModernInterfaces) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");

    EXPECT_NE(source.find("Usage: agentOS::askStream(question, [systemPrompt], [callback])"),
              std::string::npos);
    EXPECT_NE(source.find("callback: a DolphinDB function(token) called for each token chunk."),
              std::string::npos);
    EXPECT_NE(source.find("For persistent agents, prefer agentOS::askStream2(agent, question, [prompt], [callback])"),
              std::string::npos);
    EXPECT_NE(source.find("For new integrations, prefer agentOS::createAgent2(...)"),
              std::string::npos);
}

TEST(DolphinDBAdapterTest, LegacyDestroyUsagePointsToModernDestroy) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");

    EXPECT_NE(source.find("Usage: agentOS::destroyAgent(handle)"), std::string::npos);
    EXPECT_NE(source.find("For new integrations, prefer agentOS::destroy(agent)"),
              std::string::npos);
}

TEST(DolphinDBAdapterTest, LegacyAskUsagePointsToModernPersistentInterface) {
    namespace fs = std::filesystem;
    const fs::path repo_root = fs::path(__FILE__).parent_path().parent_path();

    const std::string source =
        read_file(repo_root / "plugins" / "dolphindb" / "dolphindb_adapter.cpp");

    EXPECT_NE(
        source.find("Usage: agentOS::ask(question, [systemPrompt])\\n"
                    "  For persistent agents, prefer agentOS::ask2(agent, question, [prompt])"),
        std::string::npos);
}
