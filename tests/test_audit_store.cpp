#include <agentos/bus/audit_store.hpp>
#include <agentos/bus/sqlite_audit_store.hpp>
#include <gtest/gtest.h>
#include <filesystem>

using namespace agentos::bus;
using namespace agentos;

// ── IAuditStore type tests ───────────────────────────────────

TEST(AuditEntryTest, ConstructsWithWallTimePoint) {
    AuditEntry entry{
        .id = 1,
        .timestamp = std::chrono::system_clock::now(),
        .from_agent = 10,
        .to_agent = 20,
        .type = MessageType::Request,
        .topic = "greet",
        .payload = R"({"msg":"hello"})",
        .redacted = false
    };
    EXPECT_EQ(entry.id, 1u);
    EXPECT_EQ(entry.from_agent, 10u);
    EXPECT_FALSE(entry.redacted);
}

TEST(AuditFilterTest, DefaultLimitIs100) {
    AuditFilter f;
    EXPECT_EQ(f.limit, 100u);
    EXPECT_EQ(f.offset, 0u);
    EXPECT_FALSE(f.agent_id.has_value());
}

TEST(RotationPolicyTest, DefaultValues) {
    RotationPolicy p;
    EXPECT_EQ(p.max_entries, 100000u);
    EXPECT_EQ(p.max_age.count(), 720);
}

// ── SqliteAuditStore tests ───────────────────────────────────

class SqliteAuditStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        db_path_ = (std::filesystem::temp_directory_path() / "test_audit.db").string();
        std::filesystem::remove(db_path_);
        store_ = std::make_unique<SqliteAuditStore>(db_path_);
    }
    void TearDown() override {
        store_.reset();
        // Clean up WAL/SHM files too
        std::filesystem::remove(db_path_);
        std::filesystem::remove(db_path_ + "-wal");
        std::filesystem::remove(db_path_ + "-shm");
    }
    std::string db_path_;
    std::unique_ptr<SqliteAuditStore> store_;

    AuditEntry make_entry(uint64_t id, AgentId from, AgentId to,
                          MessageType type = MessageType::Request,
                          std::string topic = "test") {
        return AuditEntry{
            .id = id,
            .timestamp = std::chrono::system_clock::now(),
            .from_agent = from,
            .to_agent = to,
            .type = type,
            .topic = std::move(topic),
            .payload = R"({"data":"hello"})",
            .redacted = false
        };
    }
};

TEST_F(SqliteAuditStoreTest, WriteAndQueryBack) {
    auto r = store_->write(make_entry(1, 10, 20));
    ASSERT_TRUE(r.has_value());
    auto results = store_->query(AuditFilter{});
    ASSERT_EQ(results.size(), 1u);
    EXPECT_EQ(results[0].from_agent, 10u);
    EXPECT_EQ(results[0].to_agent, 20u);
    EXPECT_EQ(results[0].topic, "test");
}

TEST_F(SqliteAuditStoreTest, WriteBatch) {
    std::vector<AuditEntry> entries;
    for (uint64_t i = 0; i < 5; ++i)
        entries.push_back(make_entry(i, 10, 20 + i));
    auto r = store_->write_batch(entries);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(store_->count(AuditFilter{}), 5u);
}

TEST_F(SqliteAuditStoreTest, QueryFilterByAgent) {
    (void)store_->write(make_entry(1, 10, 20));
    (void)store_->write(make_entry(2, 30, 40));
    (void)store_->write(make_entry(3, 10, 50));
    AuditFilter f;
    f.agent_id = 10;
    EXPECT_EQ(store_->query(f).size(), 2u);
}

TEST_F(SqliteAuditStoreTest, QueryFilterByType) {
    (void)store_->write(make_entry(1, 10, 20, MessageType::Request));
    (void)store_->write(make_entry(2, 10, 20, MessageType::Event));
    (void)store_->write(make_entry(3, 10, 20, MessageType::Request));
    AuditFilter f;
    f.type = MessageType::Event;
    EXPECT_EQ(store_->query(f).size(), 1u);
}

TEST_F(SqliteAuditStoreTest, QueryPagination) {
    for (uint64_t i = 0; i < 10; ++i)
        (void)store_->write(make_entry(i, 10, 20));
    AuditFilter f;
    f.limit = 3;
    f.offset = 2;
    EXPECT_EQ(store_->query(f).size(), 3u);
}

TEST_F(SqliteAuditStoreTest, RotateByMaxEntries) {
    for (uint64_t i = 0; i < 20; ++i)
        (void)store_->write(make_entry(i, 10, 20));
    EXPECT_EQ(store_->count(AuditFilter{}), 20u);
    RotationPolicy p;
    p.max_entries = 10;
    auto r = store_->rotate(p);
    ASSERT_TRUE(r.has_value());
    EXPECT_LE(store_->count(AuditFilter{}), 10u);
}

TEST_F(SqliteAuditStoreTest, FlushCheckpoint) {
    (void)store_->write(make_entry(1, 10, 20));
    // Should not throw
    store_->flush();
    EXPECT_EQ(store_->count(AuditFilter{}), 1u);
}

TEST_F(SqliteAuditStoreTest, TimestampRoundTrips) {
    auto before = std::chrono::system_clock::now();
    (void)store_->write(make_entry(1, 10, 20));
    auto results = store_->query(AuditFilter{});
    ASSERT_EQ(results.size(), 1u);
    // Timestamp should be within 1 second of 'before'
    auto diff = std::chrono::duration_cast<std::chrono::seconds>(
        results[0].timestamp - before);
    EXPECT_LE(std::abs(diff.count()), 1);
}

TEST_F(SqliteAuditStoreTest, ToAuditEntryConversion) {
    BusMessage msg = BusMessage::make_request(42, 99, "hello", R"({"x":1})");
    AuditEntry entry = to_audit_entry(msg);
    EXPECT_EQ(entry.id, msg.id);
    EXPECT_EQ(entry.from_agent, 42u);
    EXPECT_EQ(entry.to_agent, 99u);
    EXPECT_EQ(entry.type, MessageType::Request);
    EXPECT_EQ(entry.topic, "hello");
    EXPECT_FALSE(entry.redacted);
}
