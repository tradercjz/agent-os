#include "graph_engine/core/immutable_graph.hpp"
#include <filesystem>
#include <gtest/gtest.h>

namespace fs = std::filesystem;
using namespace graph_engine::core;

class ImmutableGraphTest : public ::testing::Test {
protected:
    void SetUp() override {
        test_dir = fs::temp_directory_path() / "agentos_immutable_graph_test";
        if (fs::exists(test_dir)) fs::remove_all(test_dir);
        fs::create_directories(test_dir);
    }

    void TearDown() override {
        if (fs::exists(test_dir)) fs::remove_all(test_dir);
    }

    fs::path test_dir;
};

TEST_F(ImmutableGraphTest, SubgraphResultInitialization) {
    SubgraphResult result;

    result.node_ids.push_back(1);
    result.node_ids.push_back(2);

    SubgraphResult::Triple t{"NodeA", "Knows", "NodeB", 1000, 2000};
    result.triples.push_back(t);

    EXPECT_EQ(result.node_ids.size(), 2u);
    EXPECT_EQ(result.node_ids[0], 1u);
    EXPECT_EQ(result.node_ids[1], 2u);

    EXPECT_EQ(result.triples.size(), 1u);
    EXPECT_EQ(result.triples[0].src, "NodeA");
    EXPECT_EQ(result.triples[0].relation, "Knows");
    EXPECT_EQ(result.triples[0].dst, "NodeB");
    EXPECT_EQ(result.triples[0].start_ts, 1000u);
    EXPECT_EQ(result.triples[0].end_ts, 2000u);
}

TEST_F(ImmutableGraphTest, LoadFailsOnEmptyDirectory) {
    ImmutableGraph graph(test_dir);
    // The directory is empty, so load should fail because files like entity.dict are missing
    EXPECT_FALSE(graph.load());
}

TEST_F(ImmutableGraphTest, CloseIsSafeBeforeLoad) {
    ImmutableGraph graph(test_dir);
    // It should be safe to call close even if load was never called
    EXPECT_NO_THROW(graph.close());
}

TEST_F(ImmutableGraphTest, GetNeighborsReturnsEmptyIfUnloaded) {
    ImmutableGraph graph(test_dir);
    auto neighbors = graph.get_neighbors("UnknownNode");
    EXPECT_TRUE(neighbors.empty());
}

TEST_F(ImmutableGraphTest, KHopReturnsEmptyIfUnloaded) {
    ImmutableGraph graph(test_dir);
    auto subgraph = graph.k_hop("UnknownNode", 2);
    EXPECT_TRUE(subgraph.node_ids.empty());
    EXPECT_TRUE(subgraph.triples.empty());
}

TEST_F(ImmutableGraphTest, EmptyDataMethods) {
    ImmutableGraph graph(test_dir);
    // Without load, it should be empty and gracefully handle calls
    auto neighbors = graph.get_neighbors("Apple");
    EXPECT_TRUE(neighbors.empty());

    auto incoming = graph.get_incoming_neighbors("Apple");
    EXPECT_TRUE(incoming.empty());

    auto k_hop_res = graph.k_hop("Apple", 2);
    EXPECT_TRUE(k_hop_res.node_ids.empty());
    EXPECT_TRUE(k_hop_res.triples.empty());

    EXPECT_EQ(graph.get_meta(), nullptr);
}
