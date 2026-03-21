// ============================================================
// AgentOS :: VCR Cassette Tests
// ============================================================
#include <agentos/kernel/vcr_cassette.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos;
using namespace agentos::kernel;

TEST(VCRCassetteTest, RecordAndReplay) {
    auto tmp = std::filesystem::temp_directory_path() / "test_cassette.json";

    // Record
    {
        VCRCassette cassette(tmp.string());
        cassette.record("/api/chat", R"({"model":"test"})",
                        HttpResponse{200, R"({"content":"hello"})"});
        cassette.record("/api/chat", R"({"model":"test"})",
                        HttpResponse{200, R"({"content":"world"})"});
        cassette.save();
    }

    // Replay
    {
        VCRCassette cassette(tmp.string());
        cassette.load();
        EXPECT_EQ(cassette.size(), 2u);

        auto r1 = cassette.replay();
        ASSERT_TRUE(r1.has_value());
        EXPECT_EQ(r1->status_code, 200);
        EXPECT_NE(r1->body.find("hello"), std::string::npos);

        auto r2 = cassette.replay();
        ASSERT_TRUE(r2.has_value());
        EXPECT_NE(r2->body.find("world"), std::string::npos);

        // Exhausted
        auto r3 = cassette.replay();
        EXPECT_FALSE(r3.has_value());
    }

    std::filesystem::remove(tmp);
}

TEST(VCRCassetteTest, EmptyReplayReturnsError) {
    VCRCassette cassette("/nonexistent_vcr_path");
    auto r = cassette.replay();
    EXPECT_FALSE(r.has_value());
}

TEST(VCRCassetteTest, SaveAndLoadRoundTrip) {
    auto tmp = std::filesystem::temp_directory_path() / "test_vcr_roundtrip.json";
    {
        VCRCassette c(tmp.string());
        c.record("/test", "body", HttpResponse{201, "created"});
        c.save();
    }
    {
        VCRCassette c(tmp.string());
        c.load();
        EXPECT_EQ(c.size(), 1u);
        auto r = c.replay();
        ASSERT_TRUE(r.has_value());
        EXPECT_EQ(r->status_code, 201);
        EXPECT_EQ(r->body, "created");
    }
    std::filesystem::remove(tmp);
}
