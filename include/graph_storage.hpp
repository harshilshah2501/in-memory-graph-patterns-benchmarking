#ifndef GRAPH_STORAGE_HPP
#define GRAPH_STORAGE_HPP

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <system_error>
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

namespace detail {

constexpr std::uint64_t kHashSnapshotFnvOffset = 14695981039346656037ULL;
constexpr std::uint64_t kHashSnapshotFnvPrime = 1099511628211ULL;

inline void update_hash_snapshot_checksum(std::uint64_t& checksum,
                                          std::uint8_t byte) {
    checksum ^= byte;
    checksum *= kHashSnapshotFnvPrime;
}

inline void checksum_hash_snapshot_value(std::uint64_t& checksum,
                                         std::uint64_t value,
                                         unsigned int byte_count) {
    for (unsigned int byte = 0; byte < byte_count; ++byte) {
        update_hash_snapshot_checksum(
            checksum, static_cast<std::uint8_t>(value >> (byte * 8)));
    }
}

inline void write_hash_snapshot_u32(std::ostream& output,
                                    std::uint32_t value) {
    for (unsigned int byte = 0; byte < 4; ++byte) {
        output.put(static_cast<char>(value >> (byte * 8)));
    }
}

inline void write_hash_snapshot_u64(std::ostream& output,
                                    std::uint64_t value) {
    for (unsigned int byte = 0; byte < 8; ++byte) {
        output.put(static_cast<char>(value >> (byte * 8)));
    }
}

inline std::uint32_t read_hash_snapshot_u32(std::istream& input) {
    std::uint32_t value = 0;
    for (unsigned int byte = 0; byte < 4; ++byte) {
        const int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated hash snapshot payload");
        }
        value |= static_cast<std::uint32_t>(
                     static_cast<unsigned char>(next))
                 << (byte * 8);
    }
    return value;
}

inline std::uint64_t read_hash_snapshot_u64(std::istream& input) {
    std::uint64_t value = 0;
    for (unsigned int byte = 0; byte < 8; ++byte) {
        const int next = input.get();
        if (next == std::char_traits<char>::eof()) {
            throw std::runtime_error("truncated hash snapshot payload");
        }
        value |= static_cast<std::uint64_t>(
                     static_cast<unsigned char>(next))
                 << (byte * 8);
    }
    return value;
}

template <typename Adjacency>
void write_hash_snapshot_section(std::ostream& output,
                                 const Adjacency& adjacency,
                                 std::size_t vertex_count,
                                 std::uint64_t& checksum) {
    for (std::size_t id = 0; id < vertex_count; ++id) {
        const auto& neighbors = adjacency.at(static_cast<Vertex>(id));
        write_hash_snapshot_u64(output, neighbors.size());
        checksum_hash_snapshot_value(checksum, neighbors.size(), 8);
        for (const Neighbor& neighbor : neighbors) {
            write_hash_snapshot_u32(output, neighbor.vertex);
            output.put(static_cast<char>(neighbor.type));
            output.put(static_cast<char>(neighbor.type >> 8));
            checksum_hash_snapshot_value(checksum, neighbor.vertex, 4);
            checksum_hash_snapshot_value(checksum, neighbor.type, 2);
        }
    }
}

template <typename Adjacency>
void checksum_hash_snapshot_section(const Adjacency& adjacency,
                                    std::size_t vertex_count,
                                    std::uint64_t& checksum) {
    for (std::size_t id = 0; id < vertex_count; ++id) {
        const auto& neighbors = adjacency.at(static_cast<Vertex>(id));
        checksum_hash_snapshot_value(checksum, neighbors.size(), 8);
        for (const Neighbor& neighbor : neighbors) {
            checksum_hash_snapshot_value(checksum, neighbor.vertex, 4);
            checksum_hash_snapshot_value(checksum, neighbor.type, 2);
        }
    }
}

template <typename Outgoing, typename Incoming>
void save_hash_snapshot(const std::string& path,
                        const char (&magic)[8],
                        std::size_t vertex_count,
                        std::size_t edge_count,
                        const Outgoing& outgoing,
                        const Incoming* incoming) {
    std::uint64_t checksum = kHashSnapshotFnvOffset;
    checksum_hash_snapshot_section(outgoing, vertex_count, checksum);
    if (incoming != nullptr) {
        checksum_hash_snapshot_section(*incoming, vertex_count, checksum);
    }

    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot open hash snapshot for writing: " + path);
    }
    output.write(magic, sizeof(magic));
    write_hash_snapshot_u32(output, 1);
    write_hash_snapshot_u64(output, vertex_count);
    write_hash_snapshot_u64(output, edge_count);
    write_hash_snapshot_u64(output, checksum);
    write_hash_snapshot_section(output, outgoing, vertex_count, checksum);
    if (incoming != nullptr) {
        write_hash_snapshot_section(output, *incoming, vertex_count, checksum);
    }
    if (!output) {
        throw std::runtime_error("failed writing hash snapshot: " + path);
    }
}

template <typename Outgoing, typename Incoming>
void load_hash_snapshot(const std::string& path,
                        const char (&expected_magic)[8],
                        std::size_t max_vertices,
                        std::size_t max_edges,
                        bool has_incoming,
                        std::size_t& vertex_count,
                        std::size_t& edge_count,
                        Outgoing& outgoing,
                        Incoming* incoming) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open hash snapshot for reading: " + path);
    }
    char magic[8]{};
    input.read(magic, sizeof(magic));
    if (!input || !std::equal(std::begin(magic), std::end(magic),
                              std::begin(expected_magic))) {
        throw std::runtime_error("invalid hash snapshot magic");
    }
    if (read_hash_snapshot_u32(input) != 1) {
        throw std::runtime_error("unsupported hash snapshot version");
    }
    const std::uint64_t stored_vertices = read_hash_snapshot_u64(input);
    const std::uint64_t stored_edges = read_hash_snapshot_u64(input);
    const std::uint64_t expected_checksum = read_hash_snapshot_u64(input);
    const std::uint64_t max_vertex_count =
        static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
    if (stored_vertices == 0 || stored_vertices > max_vertices ||
        stored_vertices > max_vertex_count || stored_edges > max_edges ||
        stored_vertices > std::numeric_limits<std::size_t>::max() ||
        stored_edges > std::numeric_limits<std::size_t>::max()) {
        throw std::runtime_error("hash snapshot exceeds configured limits");
    }
    if (has_incoming != (incoming != nullptr)) {
        throw std::logic_error("hash snapshot adjacency configuration mismatch");
    }
    const std::uint64_t section_count = has_incoming ? 2 : 1;
    constexpr std::uint64_t header_bytes = 36;
    constexpr std::uint64_t degree_bytes = 8;
    constexpr std::uint64_t neighbor_bytes = sizeof(Vertex) + sizeof(EdgeType);
    if (stored_vertices >
            (std::numeric_limits<std::uint64_t>::max() - header_bytes) /
                (section_count * degree_bytes) ||
        stored_edges >
            (std::numeric_limits<std::uint64_t>::max() - header_bytes -
             section_count * stored_vertices * degree_bytes) /
                (section_count * neighbor_bytes)) {
        throw std::runtime_error("hash snapshot size overflows format limits");
    }
    const std::uint64_t expected_size =
        header_bytes + section_count *
                           (stored_vertices * degree_bytes +
                            stored_edges * neighbor_bytes);
    input.seekg(0, std::ios::end);
    const std::streamoff actual_size = input.tellg();
    if (actual_size < 0 ||
        static_cast<std::uint64_t>(actual_size) != expected_size) {
        throw std::runtime_error("hash snapshot has an invalid file size");
    }
    input.seekg(header_bytes, std::ios::beg);

    vertex_count = static_cast<std::size_t>(stored_vertices);
    edge_count = static_cast<std::size_t>(stored_edges);
    outgoing.reserve(vertex_count);
    if (incoming != nullptr) {
        incoming->reserve(vertex_count);
    }
    std::uint64_t checksum = kHashSnapshotFnvOffset;
    const auto read_section = [&](auto& adjacency) {
        std::uint64_t total_neighbors = 0;
        for (std::size_t id = 0; id < vertex_count; ++id) {
            const std::uint64_t degree = read_hash_snapshot_u64(input);
            checksum_hash_snapshot_value(checksum, degree, 8);
            if (degree > stored_edges - total_neighbors) {
                throw std::runtime_error("hash snapshot degree exceeds edge count");
            }
            std::vector<Neighbor> neighbors;
            neighbors.reserve(static_cast<std::size_t>(degree));
            for (std::uint64_t index = 0; index < degree; ++index) {
                const Vertex neighbor = read_hash_snapshot_u32(input);
                const int low = input.get();
                const int high = input.get();
                if (low == std::char_traits<char>::eof() ||
                    high == std::char_traits<char>::eof() ||
                    neighbor >= vertex_count) {
                    throw std::runtime_error("hash snapshot has an invalid neighbor");
                }
                const auto type = static_cast<EdgeType>(
                    static_cast<std::uint8_t>(low) |
                    (static_cast<std::uint16_t>(
                         static_cast<std::uint8_t>(high))
                     << 8));
                neighbors.push_back(Neighbor{neighbor, type});
                checksum_hash_snapshot_value(checksum, neighbor, 4);
                checksum_hash_snapshot_value(checksum, type, 2);
            }
            total_neighbors += degree;
            adjacency.emplace(static_cast<Vertex>(id), std::move(neighbors));
        }
        if (total_neighbors != stored_edges) {
            throw std::runtime_error("hash snapshot adjacency edge count mismatch");
        }
    };
    read_section(outgoing);
    if (incoming != nullptr) {
        read_section(*incoming);
    }
    if (checksum != expected_checksum) {
        throw std::runtime_error("hash snapshot checksum mismatch");
    }
}

}  // namespace detail

class CsrCscGraph {
public:
    static constexpr std::uint32_t kSnapshotVersion = 1;

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

    void save_snapshot(const std::string& path) const {
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open snapshot for writing: " + path);
        }

        constexpr char magic[] = {'G', 'C', 'S', 'R', 'C', 'S', '1', '\0'};
        output.write(magic, sizeof(magic));
        write_u32(output, kSnapshotVersion);
        write_u64(output, vertex_count_);
        write_u64(output, edge_count_);
        write_u64(output, snapshot_checksum());
        for (std::size_t value : out_offsets_) {
            write_u64(output, value);
        }
        write_neighbors(output, out_neighbors_);
        for (std::size_t value : in_offsets_) {
            write_u64(output, value);
        }
        write_neighbors(output, in_neighbors_);
        if (!output) {
            throw std::runtime_error("failed writing snapshot: " + path);
        }
    }

    static CsrCscGraph load_snapshot(const std::string& path,
                                    std::size_t max_vertices,
                                    std::size_t max_edges) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open snapshot for reading: " + path);
        }

        constexpr char expected_magic[] = {'G', 'C', 'S', 'R', 'C', 'S', '1', '\0'};
        char magic[sizeof(expected_magic)]{};
        input.read(magic, sizeof(magic));
        if (!input || !std::equal(std::begin(magic), std::end(magic),
                                  std::begin(expected_magic))) {
            throw std::runtime_error("invalid CSR snapshot magic");
        }
        if (read_u32(input) != kSnapshotVersion) {
            throw std::runtime_error("unsupported CSR snapshot version");
        }
        const std::uint64_t vertex_count = read_u64(input);
        const std::uint64_t edge_count = read_u64(input);
        const std::uint64_t expected_checksum = read_u64(input);
        const std::uint64_t max_vertex_id_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (vertex_count > max_vertices || vertex_count > max_vertex_id_count ||
            edge_count > max_edges ||
            vertex_count > std::numeric_limits<std::size_t>::max() ||
            edge_count > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("CSR snapshot exceeds configured limits");
        }

        const std::uint64_t offset_count = vertex_count + 1;
        constexpr std::uint64_t header_bytes = 36;
        constexpr std::uint64_t offset_bytes = sizeof(std::uint64_t) * 2;
        constexpr std::uint64_t neighbor_bytes =
            (sizeof(Vertex) + sizeof(EdgeType)) * 2;
        if (offset_count >
                (std::numeric_limits<std::uint64_t>::max() - header_bytes) /
                    offset_bytes ||
            edge_count >
                (std::numeric_limits<std::uint64_t>::max() - header_bytes -
                 offset_count * offset_bytes) /
                    neighbor_bytes) {
            throw std::runtime_error("CSR snapshot size overflows format limits");
        }
        const std::uint64_t expected_size =
            header_bytes + offset_count * offset_bytes + edge_count * neighbor_bytes;
        input.seekg(0, std::ios::end);
        const std::streamoff actual_size = input.tellg();
        if (actual_size < 0 || static_cast<std::uint64_t>(actual_size) != expected_size) {
            throw std::runtime_error("CSR snapshot has an invalid file size");
        }
        input.seekg(header_bytes, std::ios::beg);

        CsrCscGraph graph;
        graph.vertex_count_ = static_cast<std::size_t>(vertex_count);
        graph.edge_count_ = static_cast<std::size_t>(edge_count);
        graph.out_offsets_.resize(static_cast<std::size_t>(offset_count));
        graph.out_neighbors_.resize(graph.edge_count_);
        graph.in_offsets_.resize(static_cast<std::size_t>(offset_count));
        graph.in_neighbors_.resize(graph.edge_count_);
        std::uint64_t checksum = kFnvOffset;
        for (std::size_t& value : graph.out_offsets_) {
            const std::uint64_t stored_value = read_hashed_u64(input, checksum);
            if (stored_value > edge_count) {
                throw std::runtime_error("CSR snapshot offset exceeds edge count");
            }
            value = static_cast<std::size_t>(stored_value);
        }
        read_hashed_neighbors(input, graph.out_neighbors_, checksum);
        for (std::size_t& value : graph.in_offsets_) {
            const std::uint64_t stored_value = read_hashed_u64(input, checksum);
            if (stored_value > edge_count) {
                throw std::runtime_error("CSC snapshot offset exceeds edge count");
            }
            value = static_cast<std::size_t>(stored_value);
        }
        read_hashed_neighbors(input, graph.in_neighbors_, checksum);
        if (checksum != expected_checksum) {
            throw std::runtime_error("CSR snapshot checksum mismatch");
        }
        validate_offsets(graph.out_offsets_, graph.edge_count_);
        validate_offsets(graph.in_offsets_, graph.edge_count_);
        for (const Neighbor& neighbor : graph.out_neighbors_) {
            graph.validate_vertex(neighbor.vertex);
        }
        for (const Neighbor& neighbor : graph.in_neighbors_) {
            graph.validate_vertex(neighbor.vertex);
        }
        return graph;
    }

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

    static constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    static void update_checksum(std::uint64_t& checksum, std::uint8_t byte) {
        checksum ^= byte;
        checksum *= kFnvPrime;
    }

    static std::uint64_t snapshot_checksum_value(std::uint64_t checksum,
                                                std::uint64_t value,
                                                unsigned int byte_count) {
        for (unsigned int byte = 0; byte < byte_count; ++byte) {
            update_checksum(checksum,
                            static_cast<std::uint8_t>(value >> (byte * 8)));
        }
        return checksum;
    }

    std::uint64_t snapshot_checksum() const {
        std::uint64_t checksum = kFnvOffset;
        for (std::size_t value : out_offsets_) {
            checksum = snapshot_checksum_value(checksum, value, 8);
        }
        for (const Neighbor& neighbor : out_neighbors_) {
            checksum = snapshot_checksum_value(checksum, neighbor.vertex, 4);
            checksum = snapshot_checksum_value(checksum, neighbor.type, 2);
        }
        for (std::size_t value : in_offsets_) {
            checksum = snapshot_checksum_value(checksum, value, 8);
        }
        for (const Neighbor& neighbor : in_neighbors_) {
            checksum = snapshot_checksum_value(checksum, neighbor.vertex, 4);
            checksum = snapshot_checksum_value(checksum, neighbor.type, 2);
        }
        return checksum;
    }

    static void write_u32(std::ostream& output, std::uint32_t value) {
        for (unsigned int byte = 0; byte < 4; ++byte) {
            output.put(static_cast<char>(value >> (byte * 8)));
        }
    }

    static void write_u64(std::ostream& output, std::uint64_t value) {
        for (unsigned int byte = 0; byte < 8; ++byte) {
            output.put(static_cast<char>(value >> (byte * 8)));
        }
    }

    static std::uint32_t read_u32(std::istream& input) {
        std::uint32_t value = 0;
        for (unsigned int byte = 0; byte < 4; ++byte) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                throw std::runtime_error("truncated CSR snapshot header");
            }
            value |= static_cast<std::uint32_t>(
                         static_cast<unsigned char>(next))
                     << (byte * 8);
        }
        return value;
    }

    static std::uint64_t read_u64(std::istream& input) {
        std::uint64_t value = 0;
        for (unsigned int byte = 0; byte < 8; ++byte) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                throw std::runtime_error("truncated CSR snapshot header");
            }
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(next))
                     << (byte * 8);
        }
        return value;
    }

    static std::uint64_t read_hashed_u64(std::istream& input,
                                         std::uint64_t& checksum) {
        const std::uint64_t value = read_u64(input);
        checksum = snapshot_checksum_value(checksum, value, 8);
        return value;
    }

    static void write_neighbors(std::ostream& output,
                                const std::vector<Neighbor>& neighbors) {
        for (const Neighbor& neighbor : neighbors) {
            write_u32(output, neighbor.vertex);
            for (unsigned int byte = 0; byte < 2; ++byte) {
                output.put(static_cast<char>(neighbor.type >> (byte * 8)));
            }
        }
    }

    static void read_hashed_neighbors(std::istream& input,
                                     std::vector<Neighbor>& neighbors,
                                     std::uint64_t& checksum) {
        for (Neighbor& neighbor : neighbors) {
            neighbor.vertex = read_u32(input);
            checksum = snapshot_checksum_value(checksum, neighbor.vertex, 4);
            std::uint16_t type = 0;
            for (unsigned int byte = 0; byte < 2; ++byte) {
                const int next = input.get();
                if (next == std::char_traits<char>::eof()) {
                    throw std::runtime_error("truncated CSR snapshot payload");
                }
                const auto value = static_cast<std::uint8_t>(next);
                type |= static_cast<std::uint16_t>(value) << (byte * 8);
                update_checksum(checksum, value);
            }
            neighbor.type = type;
        }
    }

    static void validate_offsets(const std::vector<std::size_t>& offsets,
                                 std::size_t edge_count) {
        if (offsets.empty() || offsets.front() != 0 ||
            offsets.back() != edge_count) {
            throw std::runtime_error("CSR snapshot has invalid offsets");
        }
        for (std::size_t index = 1; index < offsets.size(); ++index) {
            if (offsets[index] < offsets[index - 1] ||
                offsets[index] > edge_count) {
                throw std::runtime_error("CSR snapshot offsets are not monotonic");
            }
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
    static constexpr std::uint32_t kSnapshotVersion = 1;

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

    void save_snapshot(const std::string& path) const {
        constexpr char magic[] = {'G', 'F', 'H', 'A', 'S', 'H', '1', '\0'};
        detail::save_hash_snapshot(path, magic, vertex_count_, edge_count_,
                                   outgoing_,
                                   static_cast<const decltype(outgoing_)*>(nullptr));
    }

    static ForwardHashGraph load_snapshot(const std::string& path,
                                          std::size_t max_vertices,
                                          std::size_t max_edges) {
        constexpr char magic[] = {'G', 'F', 'H', 'A', 'S', 'H', '1', '\0'};
        ForwardHashGraph graph;
        detail::load_hash_snapshot(path, magic, max_vertices, max_edges, false,
                                   graph.vertex_count_, graph.edge_count_,
                                   graph.outgoing_,
                                   static_cast<decltype(graph.outgoing_)*>(nullptr));
        return graph;
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
    static constexpr std::uint32_t kSnapshotVersion = 1;

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

    void save_snapshot(const std::string& path) const {
        constexpr char magic[] = {'G', 'B', 'H', 'A', 'S', 'H', '1', '\0'};
        detail::save_hash_snapshot(path, magic, vertex_count_, edge_count_,
                                   outgoing_, &incoming_);
    }

    static BidirectionalHashGraph load_snapshot(const std::string& path,
                                                std::size_t max_vertices,
                                                std::size_t max_edges) {
        constexpr char magic[] = {'G', 'B', 'H', 'A', 'S', 'H', '1', '\0'};
        BidirectionalHashGraph graph;
        detail::load_hash_snapshot(path, magic, max_vertices, max_edges, true,
                                   graph.vertex_count_, graph.edge_count_,
                                   graph.outgoing_, &graph.incoming_);
        return graph;
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
