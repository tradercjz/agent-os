#include <agentos/kernel/async_http_client.hpp>
#include <gtest/gtest.h>
#include <memory>
#include <vector>

using namespace agentos;
using namespace agentos::kernel;

TEST(AsyncHttpClientTest, PostBlockingToUnreachableReturnsError) {
    AsyncHttpClient client;
    auto result = client.post_blocking("http://localhost:1/test", "{}", {}, 2);
    EXPECT_FALSE(result.has_value());
}

TEST(AsyncHttpClientTest, MultipleConcurrentBlockingRequests) {
    AsyncHttpClient client;

    // Launch several requests in parallel via threads
    std::vector<std::future<Result<HttpResponse>>> futures;
    for (int i = 0; i < 5; ++i) {
        futures.push_back(std::async(std::launch::async, [&client]() {
            return client.post_blocking("http://localhost:1/test", "{}", {}, 1);
        }));
    }

    for (auto& f : futures) {
        auto r = f.get();
        EXPECT_FALSE(r.has_value()); // all should fail (connection refused)
    }
}

TEST(AsyncHttpClientTest, ActiveRequestsCountStartsAtZero) {
    AsyncHttpClient client;
    EXPECT_EQ(client.active_requests(), 0u);
}

TEST(AsyncHttpClientTest, StopGracefully) {
    auto client = std::make_unique<AsyncHttpClient>();
    client->stop();
    // Double stop should not crash
    client->stop();
    client.reset(); // destructor after stop should not crash
}

TEST(AsyncHttpClientTest, StopBeforeAnyRequest) {
    AsyncHttpClient client;
    client.stop();
    // post_blocking after stop -- should fail cleanly or hang-free
    // (the event loop is dead, but the future should still resolve
    //  because stop() drains new_requests_)
}

TEST(AsyncHttpClientTest, PostBlockingWithHeaders) {
    AsyncHttpClient client;
    auto result = client.post_blocking(
        "http://localhost:1/test", "{}",
        {"Content-Type: application/json", "Authorization: Bearer test"}, 1);
    EXPECT_FALSE(result.has_value()); // connection refused is expected
}
