#include <gtest/gtest.h>
#include <agentos/core/compat.hpp>

using namespace agentos;

// ─────────────────────────────────────────────────────────────
// Expected<T, E> Tests
// ─────────────────────────────────────────────────────────────

TEST(ExpectedTest, ValueConstructionAndAccess) {
    Expected<int, std::string> e(42);
    EXPECT_TRUE(e.has_value());
    EXPECT_TRUE(static_cast<bool>(e));
    EXPECT_EQ(e.value(), 42);
    EXPECT_EQ(*e, 42);
    EXPECT_EQ(e.value_or(100), 42);
}

TEST(ExpectedTest, ErrorConstructionAndAccess) {
    Expected<int, std::string> e(make_unexpected(std::string("error")));
    EXPECT_FALSE(e.has_value());
    EXPECT_FALSE(static_cast<bool>(e));
    EXPECT_EQ(e.error(), "error");
    EXPECT_EQ(e.value_or(100), 100);
    EXPECT_THROW(e.value(), std::runtime_error);
}

TEST(ExpectedTest, ArrowOperator) {
    struct TestStruct { int x; };
    Expected<TestStruct, std::string> e(TestStruct{42});
    EXPECT_EQ(e->x, 42);
}

// ─────────────────────────────────────────────────────────────
// Expected<T, E> Monadic Operations Tests
// ─────────────────────────────────────────────────────────────

TEST(ExpectedTest, AndThen) {
    Expected<int, std::string> e1(42);
    auto e2 = e1.and_then([](int v) -> Expected<std::string, std::string> {
        return std::to_string(v);
    });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(e2.value(), "42");

    Expected<int, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.and_then([](int v) -> Expected<std::string, std::string> {
        return std::to_string(v);
    });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error");
}

TEST(ExpectedTest, Transform) {
    Expected<int, std::string> e1(42);
    auto e2 = e1.transform([](int v) { return v * 2; });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(e2.value(), 84);

    Expected<int, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.transform([](int v) { return v * 2; });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error");
}

TEST(ExpectedTest, TransformVoid) {
    int count = 0;
    Expected<int, std::string> e1(42);
    auto e2 = e1.transform([&count](int v) { count += v; });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(count, 42);

    Expected<int, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.transform([&count](int v) { count += v; });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error");
    EXPECT_EQ(count, 42); // count should not change
}

TEST(ExpectedTest, OrElse) {
    Expected<int, std::string> e1(42);
    auto e2 = e1.or_else([](const std::string& err) -> Expected<int, std::string> {
        return make_unexpected(err + "_handled");
    });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(e2.value(), 42);

    Expected<int, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.or_else([](const std::string& e) -> Expected<int, std::string> {
        return make_unexpected(e + "_handled");
    });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error_handled");

    auto e4 = err.or_else([](const std::string&) -> Expected<int, std::string> {
        return 100;
    });
    EXPECT_TRUE(e4.has_value());
    EXPECT_EQ(e4.value(), 100);
}

// ─────────────────────────────────────────────────────────────
// Expected<void, E> Tests
// ─────────────────────────────────────────────────────────────

TEST(ExpectedVoidTest, ValueAndError) {
    Expected<void, std::string> e1;
    EXPECT_TRUE(e1.has_value());
    EXPECT_NO_THROW(e1.value());

    Expected<void, std::string> e2(make_unexpected(std::string("error")));
    EXPECT_FALSE(e2.has_value());
    EXPECT_EQ(e2.error(), "error");
    EXPECT_THROW(e2.value(), std::runtime_error);
}

TEST(ExpectedVoidTest, AndThen) {
    Expected<void, std::string> e1;
    auto e2 = e1.and_then([]() -> Expected<int, std::string> {
        return 42;
    });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(e2.value(), 42);

    Expected<void, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.and_then([]() -> Expected<int, std::string> {
        return 42;
    });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error");
}

TEST(ExpectedVoidTest, Transform) {
    Expected<void, std::string> e1;
    auto e2 = e1.transform([]() { return 42; });
    EXPECT_TRUE(e2.has_value());
    EXPECT_EQ(e2.value(), 42);

    Expected<void, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.transform([]() { return 42; });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error");
}

TEST(ExpectedVoidTest, OrElse) {
    Expected<void, std::string> e1;
    auto e2 = e1.or_else([](const std::string& err) -> Expected<void, std::string> {
        return make_unexpected(err + "_handled");
    });
    EXPECT_TRUE(e2.has_value());

    Expected<void, std::string> err(make_unexpected(std::string("error")));
    auto e3 = err.or_else([](const std::string& e) -> Expected<void, std::string> {
        return make_unexpected(e + "_handled");
    });
    EXPECT_FALSE(e3.has_value());
    EXPECT_EQ(e3.error(), "error_handled");

    auto e4 = err.or_else([](const std::string&) -> Expected<void, std::string> {
        return Expected<void, std::string>();
    });
    EXPECT_TRUE(e4.has_value());
}

// ─────────────────────────────────────────────────────────────
// fmt::format Tests
// ─────────────────────────────────────────────────────────────

TEST(FmtFormatTest, BasicFormatting) {
    EXPECT_EQ(fmt::format("Hello {}!", "World"), "Hello World!");
    EXPECT_EQ(fmt::format("Number: {}", 42), "Number: 42");
    EXPECT_EQ(fmt::format("Bool: {}", true), "Bool: true");
    EXPECT_EQ(fmt::format("Bool: {}", false), "Bool: false");
    EXPECT_EQ(fmt::format("{} + {} = {}", 1, 2, 3), "1 + 2 = 3");
}

TEST(FmtFormatTest, MissingArgsOrPlaceholders) {
    EXPECT_EQ(fmt::format("No placeholders", 42), "No placeholders");
    EXPECT_EQ(fmt::format("Missing {}", "args", "extra"), "Missing args");
    EXPECT_EQ(fmt::format("Extra {}{}"), "Extra {}{}");
    EXPECT_EQ(fmt::format("Incomplete {"), "Incomplete {");
}

// ─────────────────────────────────────────────────────────────
// SourceLocation Tests
// ─────────────────────────────────────────────────────────────

TEST(SourceLocationTest, BasicInfo) {
    auto loc = SourceLocation::current();
    EXPECT_NE(loc.file_name(), nullptr);
    EXPECT_GT(loc.line_number(), 0);

    // Check if file name looks like a string
    std::string file(loc.file_name());
    EXPECT_FALSE(file.empty());
}
