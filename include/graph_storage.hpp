#ifndef GRAPH_STORAGE_HPP
#define GRAPH_STORAGE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

namespace graph_storage {

using Vertex = std::uint32_t;
using EdgeType = std::uint16_t;

struct Edge {
    Vertex source;
    Vertex target;
    EdgeType type;
};

struct Neighbor {
    Vertex vertex;
    EdgeType type;

    bool operator==(const Neighbor& other) const {
        return vertex == other.vertex && type == other.type;
    }
};

class NeighborRange {
public:
    using ConstIterator = std::vector<Neighbor>::const_iterator;

    NeighborRange(const std::vector<Neighbor>& values,
                  std::size_t begin,
                  std::size_t end)
        : values_(values), begin_(begin), end_(end) {}

    ConstIterator begin() const { return values_.begin() + begin_; }
    ConstIterator end() const { return values_.begin() + end_; }
    std::size_t size() const { return end_ - begin_; }
    bool empty() const { return begin_ == end_; }

private:
    const std::vector<Neighbor>& values_;
    std::size_t begin_;
    std::size_t end_;
};

class CsrCscGraph {
public:
    static CsrCscGraph build(std::size_t vertex_count,
                             const std::vector<Edge>& edges) {
        const std::uint64_t max_vertex_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (vertex_count > max_vertex_count) {
            throw std::length_error("vertex count exceeds the Vertex id range");
        }

        CsrCscGraph graph;
        graph.vertex_count_ = vertex_count;
        graph.edge_count_ = edges.size();
        graph.out_offsets_.assign(vertex_count + 1, 0);
        graph.in_offsets_.assign(vertex_count + 1, 0);

        for (const Edge& edge : edges) {
            graph.validate_vertex(edge.source);
            graph.validate_vertex(edge.target);
            ++graph.out_offsets_[static_cast<std::size_t>(edge.source) + 1];
            ++graph.in_offsets_[static_cast<std::size_t>(edge.target) + 1];
        }

        prefix_sum(graph.out_offsets_);
        prefix_sum(graph.in_offsets_);
        graph.out_neighbors_.resize(edges.size());
        graph.in_neighbors_.resize(edges.size());

        std::vector<std::size_t> out_cursor(graph.out_offsets_.begin(),
                                            graph.out_offsets_.end() - 1);
        std::vector<std::size_t> in_cursor(graph.in_offsets_.begin(),
                                           graph.in_offsets_.end() - 1);
        for (const Edge& edge : edges) {
            graph.out_neighbors_[out_cursor[edge.source]++] =
                Neighbor{edge.target, edge.type};
            graph.in_neighbors_[in_cursor[edge.target]++] =
                Neighbor{edge.source, edge.type};
        }

        const auto neighbor_order = [](const Neighbor& left,
                                       const Neighbor& right) {
            if (left.type != right.type) {
                return left.type < right.type;
            }
            return left.vertex < right.vertex;
        };
        sort_ranges(graph.out_offsets_, graph.out_neighbors_, neighbor_order);
        sort_ranges(graph.in_offsets_, graph.in_neighbors_, neighbor_order);
        return graph;
    }

    std::size_t vertex_count() const { return vertex_count_; }
    std::size_t edge_count() const { return edge_count_; }

    NeighborRange out_neighbors(Vertex vertex) const {
        validate_vertex(vertex);
        const std::size_t id = vertex;
        return NeighborRange(out_neighbors_, out_offsets_[id],
                             out_offsets_[id + 1]);
    }

    NeighborRange in_neighbors(Vertex vertex) const {
        validate_vertex(vertex);
        const std::size_t id = vertex;
        return NeighborRange(in_neighbors_, in_offsets_[id],
                             in_offsets_[id + 1]);
    }

    std::size_t storage_bytes() const {
        return out_offsets_.capacity() * sizeof(std::size_t) +
               in_offsets_.capacity() * sizeof(std::size_t) +
               out_neighbors_.capacity() * sizeof(Neighbor) +
               in_neighbors_.capacity() * sizeof(Neighbor);
    }

    std::size_t retained_index_bytes_estimate() const { return storage_bytes(); }

private:
    CsrCscGraph() = default;

    void validate_vertex(Vertex vertex) const {
        if (static_cast<std::size_t>(vertex) >= vertex_count_) {
            throw std::out_of_range("edge or query vertex is out of range");
        }
    }

    static void prefix_sum(std::vector<std::size_t>& offsets) {
        for (std::size_t index = 1; index < offsets.size(); ++index) {
            offsets[index] += offsets[index - 1];
        }
    }

    template <typename Compare>
    static void sort_ranges(const std::vector<std::size_t>& offsets,
                            std::vector<Neighbor>& neighbors,
                            Compare compare) {
        for (std::size_t vertex = 0; vertex + 1 < offsets.size(); ++vertex) {
            std::sort(neighbors.begin() + offsets[vertex],
                      neighbors.begin() + offsets[vertex + 1], compare);
        }
    }

    std::size_t vertex_count_ = 0;
    std::size_t edge_count_ = 0;
    std::vector<std::size_t> out_offsets_;
    std::vector<Neighbor> out_neighbors_;
    std::vector<std::size_t> in_offsets_;
    std::vector<Neighbor> in_neighbors_;
};

class ForwardHashGraph {
public:
    static ForwardHashGraph build(std::size_t vertex_count,
                                  const std::vector<Edge>& edges) {
        const std::uint64_t max_vertex_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (vertex_count > max_vertex_count) {
            throw std::length_error("vertex count exceeds the Vertex id range");
        }

        ForwardHashGraph graph;
        graph.vertex_count_ = vertex_count;
        graph.edge_count_ = edges.size();
        graph.outgoing_.reserve(vertex_count);
        for (std::size_t id = 0; id < vertex_count; ++id) {
            graph.outgoing_.emplace(static_cast<Vertex>(id),
                                    std::vector<Neighbor>{});
        }

        for (const Edge& edge : edges) {
            graph.validate_vertex(edge.source);
            graph.validate_vertex(edge.target);
            graph.outgoing_.at(edge.source).push_back(
                Neighbor{edge.target, edge.type});
        }
        return graph;
    }

    std::size_t vertex_count() const { return vertex_count_; }
    std::size_t edge_count() const { return edge_count_; }

    const std::vector<Neighbor>& out_neighbors(Vertex vertex) const {
        validate_vertex(vertex);
        return outgoing_.at(vertex);
    }

    std::size_t retained_index_bytes_estimate() const {
        return estimate_adjacency_map_bytes(outgoing_);
    }

private:
    static std::size_t estimate_adjacency_map_bytes(
        const std::unordered_map<Vertex, std::vector<Neighbor>>& adjacency) {
        std::size_t bytes = 0;
        bytes += adjacency.bucket_count() * sizeof(void*);
        bytes += adjacency.size() * sizeof(std::pair<const Vertex, std::vector<Neighbor>>);
        bytes += adjacency.size() * (2 * sizeof(void*) + sizeof(std::size_t));
        for (const auto& entry : adjacency) {
            bytes += entry.second.capacity() * sizeof(Neighbor);
        }
        return bytes;
    }

    void validate_vertex(Vertex vertex) const {
        if (static_cast<std::size_t>(vertex) >= vertex_count_) {
            throw std::out_of_range("edge or query vertex is out of range");
        }
    }

    std::size_t vertex_count_ = 0;
    std::size_t edge_count_ = 0;
    std::unordered_map<Vertex, std::vector<Neighbor>> outgoing_;
};

class BidirectionalHashGraph {
public:
    static BidirectionalHashGraph build(std::size_t vertex_count,
                                        const std::vector<Edge>& edges) {
        const std::uint64_t max_vertex_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (vertex_count > max_vertex_count) {
            throw std::length_error("vertex count exceeds the Vertex id range");
        }

        BidirectionalHashGraph graph;
        graph.vertex_count_ = vertex_count;
        graph.edge_count_ = edges.size();
        graph.outgoing_.reserve(vertex_count);
        graph.incoming_.reserve(vertex_count);
        for (std::size_t id = 0; id < vertex_count; ++id) {
            const Vertex vertex = static_cast<Vertex>(id);
            graph.outgoing_.emplace(vertex, std::vector<Neighbor>{});
            graph.incoming_.emplace(vertex, std::vector<Neighbor>{});
        }

        std::vector<std::size_t> out_degree(vertex_count, 0);
        std::vector<std::size_t> in_degree(vertex_count, 0);
        for (const Edge& edge : edges) {
            graph.validate_vertex(edge.source);
            graph.validate_vertex(edge.target);
            ++out_degree[edge.source];
            ++in_degree[edge.target];
        }
        for (std::size_t id = 0; id < vertex_count; ++id) {
            graph.outgoing_.at(static_cast<Vertex>(id)).reserve(out_degree[id]);
            graph.incoming_.at(static_cast<Vertex>(id)).reserve(in_degree[id]);
        }
        for (const Edge& edge : edges) {
            graph.outgoing_.at(edge.source).push_back(
                Neighbor{edge.target, edge.type});
            graph.incoming_.at(edge.target).push_back(
                Neighbor{edge.source, edge.type});
        }
        return graph;
    }

    std::size_t vertex_count() const { return vertex_count_; }
    std::size_t edge_count() const { return edge_count_; }

    const std::vector<Neighbor>& out_neighbors(Vertex vertex) const {
        validate_vertex(vertex);
        return outgoing_.at(vertex);
    }

    const std::vector<Neighbor>& in_neighbors(Vertex vertex) const {
        validate_vertex(vertex);
        return incoming_.at(vertex);
    }

    std::size_t retained_index_bytes_estimate() const {
        return estimate_adjacency_map_bytes(outgoing_) +
               estimate_adjacency_map_bytes(incoming_);
    }

private:
    static std::size_t estimate_adjacency_map_bytes(
        const std::unordered_map<Vertex, std::vector<Neighbor>>& adjacency) {
        std::size_t bytes = 0;
        bytes += adjacency.bucket_count() * sizeof(void*);
        bytes += adjacency.size() * sizeof(std::pair<const Vertex, std::vector<Neighbor>>);
        bytes += adjacency.size() * (2 * sizeof(void*) + sizeof(std::size_t));
        for (const auto& entry : adjacency) {
            bytes += entry.second.capacity() * sizeof(Neighbor);
        }
        return bytes;
    }

    void validate_vertex(Vertex vertex) const {
        if (static_cast<std::size_t>(vertex) >= vertex_count_) {
            throw std::out_of_range("edge or query vertex is out of range");
        }
    }

    std::size_t vertex_count_ = 0;
    std::size_t edge_count_ = 0;
    std::unordered_map<Vertex, std::vector<Neighbor>> outgoing_;
    std::unordered_map<Vertex, std::vector<Neighbor>> incoming_;
};

class SnapshotPublisher {
public:
    using Snapshot = std::shared_ptr<const CsrCscGraph>;

    explicit SnapshotPublisher(Snapshot initial)
        : current_(std::move(initial)) {
        if (!current_) {
            throw std::invalid_argument("initial graph snapshot must not be null");
        }
    }

    Snapshot acquire() const {
        return std::atomic_load_explicit(&current_, std::memory_order_acquire);
    }

    void publish(Snapshot next) {
        if (!next) {
            throw std::invalid_argument("published graph snapshot must not be null");
        }
        std::atomic_store_explicit(&current_, std::move(next),
                                   std::memory_order_release);
    }

private:
    Snapshot current_;
};

}  // namespace graph_storage

#endif
