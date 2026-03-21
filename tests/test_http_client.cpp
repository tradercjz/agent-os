#include <agentos/kernel/http_client.hpp>
#include <gtest/gtest.h>

using namespace agentos;
using namespace agentos::kernel;

TEST(HttpClientTest, PostToInvalidUrlReturnsError) {
    HttpClient client;
    auto r = client.post("http://localhost:1/nonexistent", "{}", {}, 2);
    EXPECT_FALSE(r.has_value());
}

TEST(HttpClientTest, HeadersBuilt) {
    // Verify construction doesn't crash and handles unreachable URL
    HttpClient client;
    auto r = client.post("http://localhost:1/test", "{}",
                         {"Content-Type: application/json", "Authorization: Bearer test"}, 1);
    EXPECT_FALSE(r.has_value()); // connection refused is fine
}

TEST(HttpClientTest, PostStreamToInvalidUrlReturnsError) {
    HttpClient client;
    bool callback_called = false;
    auto r = client.post_stream("http://localhost:1/nonexistent", "{}",
                                {"Content-Type: application/json"},
                                [&](const char*, size_t len) -> size_t {
                                    callback_called = true;
                                    return len;
                                }, 2);
    EXPECT_FALSE(r.has_value());
    EXPECT_FALSE(callback_called);
}

TEST(HttpClientTest, DefaultConstruction) {
    // Just verify default construction works without crash
    HttpClient client;
    SUCCEED();
}
