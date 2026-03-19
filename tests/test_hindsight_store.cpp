#include <gtest/gtest.h>
#include <agentos/memory/hindsight_store.hpp>
#include <agentos/core/types.hpp>

using namespace agentos;
using namespace agentos::memory;

TEST(HindsightStoreTest, BasicInstantiation) {
    HindsightMemoryStore store("http://localhost:8888", "test_bank", "test_key");
    EXPECT_EQ(store.name(), "HindsightMemoryStore");
    EXPECT_EQ(store.size(), 0u);
}

TEST(HindsightStoreTest, ReadForgetSearchNotSupported) {
    HindsightMemoryStore store;

    auto read_res = store.read("test_id");
    EXPECT_FALSE(read_res.has_value());
    EXPECT_EQ(read_res.error().code, ErrorCode::MemoryReadFailed);

    auto forget_res = store.forget("test_id");
    EXPECT_FALSE(forget_res.has_value());
    EXPECT_EQ(forget_res.error().code, ErrorCode::MemoryWriteFailed);

    Embedding q_emb = {0.1f, 0.2f};
    MemoryFilter filter;
    auto search_res = store.search(q_emb, filter, 5);
    EXPECT_FALSE(search_res.has_value());
    EXPECT_EQ(search_res.error().code, ErrorCode::MemoryReadFailed);
}
