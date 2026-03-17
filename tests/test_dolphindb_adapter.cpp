#include <gtest/gtest.h>
#include <stdexcept>
#include <string>

// To test the macros from the plugin without linking the full DolphinDB SDK,
// we define simple mocks for the exception classes and include the new
// dolphindb_macros.hpp file which contains the isolated macros.

namespace ddb {
    class IllegalArgumentException : public std::exception {
    public:
        IllegalArgumentException(const std::string&, const std::string&) {}
        virtual const char* what() const noexcept override { return "IllegalArgumentException"; }
    };

    class RuntimeException : public std::exception {
    public:
        RuntimeException(const std::string& errMsg) : msg_(errMsg) {}
        virtual const char* what() const noexcept override { return msg_.c_str(); }
    private:
        std::string msg_;
    };
}

#include "../plugins/dolphindb/dolphindb_macros.hpp"

namespace agentos::dolphindb {

// Test helper function using the macros directly from dolphindb_macros.hpp
void testFunction(int type) {
    DDB_SAFE_BEGIN
        if (type == 1) throw ddb::IllegalArgumentException("test", "arg error");
        if (type == 2) throw ddb::RuntimeException("runtime error");
        if (type == 3) throw std::runtime_error("std error");
        if (type == 4) throw 42;
    DDB_SAFE_END("testFunction")
}

} // namespace agentos::dolphindb

TEST(DolphinDBAdapterTest, ExceptionHandling) {
    using namespace agentos::dolphindb;
    using namespace ddb;

    // 1. IllegalArgumentException should be rethrown as is
    EXPECT_THROW(testFunction(1), IllegalArgumentException);

    // 2. RuntimeException should be rethrown as is
    EXPECT_THROW(testFunction(2), RuntimeException);

    // 3. std::exception should be caught and thrown as ddb::RuntimeException
    try {
        testFunction(3);
        FAIL() << "Expected ddb::RuntimeException";
    } catch (const RuntimeException& e) {
        EXPECT_EQ(std::string(e.what()), "testFunction error: std error");
    }

    // 4. Unknown exception should be caught and thrown as ddb::RuntimeException
    try {
        testFunction(4);
        FAIL() << "Expected ddb::RuntimeException";
    } catch (const RuntimeException& e) {
        EXPECT_EQ(std::string(e.what()), "testFunction: unknown internal error");
    }
}
