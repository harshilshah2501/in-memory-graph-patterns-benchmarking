#include "graph_storage.hpp"
#include "graph_recovery.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

using graph_storage::CsrCscGraph;
using graph_storage::DurableEdgeSource;
using graph_storage::Edge;
using graph_storage::BidirectionalHashGraph;
using graph_storage::ForwardHashGraph;
using graph_storage::SnapshotPublisher;

namespace {

void require(bool condition, const char* message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void test_bidirectional_typed_neighbors() {
    const std::vector<Edge> edges = {
        {0, 2, 3}, {0, 1, 1}, {2, 0, 2}, {0, 1, 1}, {3, 1, 1}};
    const CsrCscGraph graph = CsrCscGraph::build(4, edges);

    require(graph.vertex_count() == 4, "vertex count mismatch");
    require(graph.edge_count() == edges.size(), "edge count mismatch");

    const auto out = graph.out_neighbors(0);
    require(out.size() == 3, "out-degree mismatch");
    auto out_it = out.begin();
    require(out_it->vertex == 1 && out_it->type == 1,
            "typed outgoing neighbors are not ordered");
    ++out_it;
    require(out_it->vertex == 1 && out_it->type == 1,
            "duplicate edges must be preserved");
    ++out_it;
    require(out_it->vertex == 2 && out_it->type == 3,
            "outgoing neighbor mismatch");

    const auto in = graph.in_neighbors(1);
    require(in.size() == 3, "in-degree mismatch");
    auto in_it = in.begin();
    require(in_it->vertex == 0 && in_it->type == 1,
            "incoming neighbor ordering mismatch");
    ++in_it;
    require(in_it->vertex == 0 && in_it->type == 1,
            "incoming duplicate edge mismatch");
    ++in_it;
    require(in_it->vertex == 3 && in_it->type == 1,
            "incoming neighbor mismatch");
}

void test_empty_graph_and_isolated_vertices() {
    const CsrCscGraph graph = CsrCscGraph::build(3, {});
    require(graph.edge_count() == 0, "empty graph has edges");
    require(graph.out_neighbors(2).empty(), "isolated vertex has outgoing edges");
    require(graph.in_neighbors(2).empty(), "isolated vertex has incoming edges");
    require(graph.storage_bytes() > 0, "offset arrays should be accounted for");
}

void test_forward_hash_reference() {
    const std::vector<Edge> edges = {{0, 1, 4}, {0, 2, 5}};
    const ForwardHashGraph graph = ForwardHashGraph::build(3, edges);
    require(graph.edge_count() == 2, "hash graph edge count mismatch");
    require(graph.out_neighbors(0).size() == 2, "hash out-degree mismatch");
    require(graph.out_neighbors(2).empty(), "hash isolated vertex mismatch");
}

void test_bidirectional_hash_matches_csr() {
    const std::vector<Edge> edges = {
        {0, 2, 3}, {0, 1, 1}, {2, 0, 2}, {0, 1, 1}, {3, 1, 1}};
    const BidirectionalHashGraph hash = BidirectionalHashGraph::build(4, edges);
    const CsrCscGraph packed = CsrCscGraph::build(4, edges);

    require(hash.edge_count() == packed.edge_count(),
            "bidirectional hash edge count differs from CSR+CSC");
    for (graph_storage::Vertex vertex = 0; vertex < 4; ++vertex) {
        std::vector<graph_storage::Neighbor> hash_out(
            hash.out_neighbors(vertex).begin(), hash.out_neighbors(vertex).end());
        std::vector<graph_storage::Neighbor> packed_out(
            packed.out_neighbors(vertex).begin(), packed.out_neighbors(vertex).end());
        const auto order = [](const auto& left, const auto& right) {
            if (left.type != right.type) {
                return left.type < right.type;
            }
            return left.vertex < right.vertex;
        };
        std::sort(hash_out.begin(), hash_out.end(), order);
        require(hash_out == packed_out,
                "outgoing hash adjacency differs from CSR adjacency");

        std::vector<graph_storage::Neighbor> hash_in(
            hash.in_neighbors(vertex).begin(), hash.in_neighbors(vertex).end());
        std::vector<graph_storage::Neighbor> packed_in(
            packed.in_neighbors(vertex).begin(), packed.in_neighbors(vertex).end());
        std::sort(hash_in.begin(), hash_in.end(), order);
        require(hash_in == packed_in,
                "incoming hash adjacency differs from CSC adjacency");
    }
}

void test_invalid_vertices_are_rejected() {
    bool rejected_edge = false;
    try {
        (void)CsrCscGraph::build(2, {{0, 2, 0}});
    } catch (const std::out_of_range&) {
        rejected_edge = true;
    }
    require(rejected_edge, "builder accepted an out-of-range edge");

    const CsrCscGraph graph = CsrCscGraph::build(2, {});
    bool rejected_query = false;
    try {
        (void)graph.out_neighbors(2);
    } catch (const std::out_of_range&) {
        rejected_query = true;
    }
    require(rejected_query, "query accepted an out-of-range vertex");
}

void test_atomic_snapshot_publication() {
    auto first = std::make_shared<const CsrCscGraph>(
        CsrCscGraph::build(2, {{0, 1, 1}}));
    SnapshotPublisher publisher(first);
    const auto pinned_old_snapshot = publisher.acquire();

    auto second = std::make_shared<const CsrCscGraph>(
        CsrCscGraph::build(2, {{1, 0, 2}}));
    publisher.publish(second);

    const auto current = publisher.acquire();
    require(current->out_neighbors(1).size() == 1,
            "published snapshot is not visible");
    require(pinned_old_snapshot->out_neighbors(0).size() == 1,
            "pinned old snapshot was modified or reclaimed");
    require(pinned_old_snapshot->out_neighbors(1).empty(),
            "old snapshot unexpectedly sees the new graph");
}

void test_durable_source_and_snapshot_round_trip() {
    const auto base = std::filesystem::temp_directory_path() /
                      "graph_storage_recovery_test";
    const auto source_path = (base.string() + "_edges.bin");
    const auto snapshot_path = (base.string() + "_snapshot.bin");
    const auto forward_snapshot_path = (base.string() + "_forward.snapshot");
    const auto bidirectional_snapshot_path =
        (base.string() + "_bidirectional.snapshot");
    const std::vector<Edge> edges = {
        {0, 2, 3}, {0, 1, 1}, {2, 0, 2}, {0, 1, 1}, {3, 1, 1}};
    const CsrCscGraph expected = CsrCscGraph::build(4, edges);

    DurableEdgeSource::write(source_path, 4, edges);
    std::size_t loaded_vertices = 0;
    const auto loaded_edges =
        DurableEdgeSource::read(source_path, 4, edges.size(), loaded_vertices);
    require(loaded_vertices == 4, "durable source vertex count mismatch");
    require(loaded_edges.size() == edges.size(),
            "durable source edge count mismatch");
    for (std::size_t index = 0; index < edges.size(); ++index) {
        require(loaded_edges[index].source == edges[index].source &&
                    loaded_edges[index].target == edges[index].target &&
                    loaded_edges[index].type == edges[index].type,
                "durable source edge mismatch");
    }

    expected.save_snapshot(snapshot_path);
    const CsrCscGraph restored =
        CsrCscGraph::load_snapshot(snapshot_path, 4, edges.size());
    require(restored.vertex_count() == expected.vertex_count(),
            "snapshot vertex count mismatch");
    require(restored.edge_count() == expected.edge_count(),
            "snapshot edge count mismatch");
    for (graph_storage::Vertex vertex = 0; vertex < 4; ++vertex) {
        const auto expected_out = expected.out_neighbors(vertex);
        const auto restored_out = restored.out_neighbors(vertex);
        require(std::equal(expected_out.begin(), expected_out.end(),
                           restored_out.begin(), restored_out.end()),
                "snapshot outgoing adjacency mismatch");
        const auto expected_in = expected.in_neighbors(vertex);
        const auto restored_in = restored.in_neighbors(vertex);
        require(std::equal(expected_in.begin(), expected_in.end(),
                           restored_in.begin(), restored_in.end()),
                "snapshot incoming adjacency mismatch");
    }

    const ForwardHashGraph forward = ForwardHashGraph::build(4, edges);
    forward.save_snapshot(forward_snapshot_path);
    const ForwardHashGraph restored_forward =
        ForwardHashGraph::load_snapshot(forward_snapshot_path, 4, edges.size());
    require(restored_forward.edge_count() == edges.size(),
            "forward-hash snapshot edge count mismatch");
    for (graph_storage::Vertex vertex = 0; vertex < 4; ++vertex) {
        require(forward.out_neighbors(vertex) ==
                    restored_forward.out_neighbors(vertex),
                "forward-hash snapshot adjacency mismatch");
    }

    const BidirectionalHashGraph bidirectional =
        BidirectionalHashGraph::build(4, edges);
    bidirectional.save_snapshot(bidirectional_snapshot_path);
    const BidirectionalHashGraph restored_bidirectional =
        BidirectionalHashGraph::load_snapshot(bidirectional_snapshot_path, 4,
                                              edges.size());
    require(restored_bidirectional.edge_count() == edges.size(),
            "bidirectional-hash snapshot edge count mismatch");
    for (graph_storage::Vertex vertex = 0; vertex < 4; ++vertex) {
        require(bidirectional.out_neighbors(vertex) ==
                    restored_bidirectional.out_neighbors(vertex),
                "bidirectional-hash snapshot outgoing adjacency mismatch");
        require(bidirectional.in_neighbors(vertex) ==
                    restored_bidirectional.in_neighbors(vertex),
                "bidirectional-hash snapshot incoming adjacency mismatch");
    }
    bool rejected_hash_limits = false;
    try {
        (void)ForwardHashGraph::load_snapshot(forward_snapshot_path, 3,
                                              edges.size());
    } catch (const std::runtime_error&) {
        rejected_hash_limits = true;
    }
    require(rejected_hash_limits,
            "hash snapshot loader ignored configured vertex limit");
    bool rejected_hash_edge_limit = false;
    try {
        (void)ForwardHashGraph::load_snapshot(forward_snapshot_path, 4,
                                              edges.size() - 1);
    } catch (const std::runtime_error&) {
        rejected_hash_edge_limit = true;
    }
    require(rejected_hash_edge_limit,
            "hash snapshot loader ignored configured edge limit");
    {
        std::fstream snapshot(forward_snapshot_path,
                              std::ios::binary | std::ios::in | std::ios::out);
        snapshot.seekp(-1, std::ios::end);
        const char byte = '\x7f';
        snapshot.write(&byte, 1);
    }
    bool rejected_hash_corruption = false;
    try {
        (void)ForwardHashGraph::load_snapshot(forward_snapshot_path, 4,
                                              edges.size());
    } catch (const std::runtime_error&) {
        rejected_hash_corruption = true;
    }
    require(rejected_hash_corruption,
            "hash snapshot loader accepted checksum corruption");

    bool rejected_limits = false;
    try {
        (void)CsrCscGraph::load_snapshot(snapshot_path, 3, edges.size());
    } catch (const std::runtime_error&) {
        rejected_limits = true;
    }
    require(rejected_limits, "snapshot loader ignored configured size limits");
    bool rejected_edge_limit = false;
    try {
        (void)CsrCscGraph::load_snapshot(snapshot_path, 4, edges.size() - 1);
    } catch (const std::runtime_error&) {
        rejected_edge_limit = true;
    }
    require(rejected_edge_limit, "snapshot loader ignored edge-count limit");

    {
        std::fstream snapshot(snapshot_path,
                              std::ios::binary | std::ios::in | std::ios::out);
        snapshot.seekp(-1, std::ios::end);
        const char byte = '\x7f';
        snapshot.write(&byte, 1);
    }
    bool rejected_corruption = false;
    try {
        (void)CsrCscGraph::load_snapshot(snapshot_path, 4, edges.size());
    } catch (const std::runtime_error&) {
        rejected_corruption = true;
    }
    require(rejected_corruption, "snapshot loader accepted checksum corruption");

    DurableEdgeSource::write(source_path, 4, edges);
    {
        std::fstream source(source_path,
                            std::ios::binary | std::ios::in | std::ios::out);
        source.seekp(-1, std::ios::end);
        const char byte = '\x7f';
        source.write(&byte, 1);
    }
    bool rejected_source_corruption = false;
    try {
        (void)DurableEdgeSource::read(source_path, 4, edges.size(),
                                      loaded_vertices);
    } catch (const std::runtime_error&) {
        rejected_source_corruption = true;
    }
    require(rejected_source_corruption,
            "durable source loader accepted checksum corruption");
    DurableEdgeSource::write(source_path, 4, edges);
    bool rejected_source_limit = false;
    try {
        (void)DurableEdgeSource::read(source_path, 4, edges.size() - 1,
                                      loaded_vertices);
    } catch (const std::runtime_error&) {
        rejected_source_limit = true;
    }
    require(rejected_source_limit,
            "durable source loader ignored edge-count limit");

    std::filesystem::remove(source_path);
    std::filesystem::remove(snapshot_path);
    std::filesystem::remove(forward_snapshot_path);
    std::filesystem::remove(bidirectional_snapshot_path);
}

}  // namespace

int main() {
    try {
        test_bidirectional_typed_neighbors();
        test_empty_graph_and_isolated_vertices();
        test_forward_hash_reference();
        test_bidirectional_hash_matches_csr();
        test_invalid_vertices_are_rejected();
        test_atomic_snapshot_publication();
        test_durable_source_and_snapshot_round_trip();
        std::cout << "All graph storage tests passed.\n";
    } catch (const std::exception& error) {
        std::cerr << "Test failure: " << error.what() << '\n';
        return 1;
    }
}
