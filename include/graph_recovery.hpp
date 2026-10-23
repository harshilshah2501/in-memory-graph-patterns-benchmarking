#ifndef GRAPH_RECOVERY_HPP
#define GRAPH_RECOVERY_HPP

#include "graph_storage.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace graph_storage {

class DurableEdgeSource {
public:
    static constexpr std::uint32_t kFormatVersion = 1;

    static void write(const std::string& path,
                      std::size_t vertex_count,
                      const std::vector<Edge>& edges) {
        validate_edges(vertex_count, edges);
        std::ofstream output(path, std::ios::binary | std::ios::trunc);
        if (!output) {
            throw std::runtime_error("cannot open edge source for writing: " + path);
        }
        constexpr char magic[] = {'G', 'E', 'D', 'G', 'E', 'S', '1', '\0'};
        output.write(magic, sizeof(magic));
        write_u32(output, kFormatVersion);
        write_u64(output, vertex_count);
        write_u64(output, edges.size());
        write_u64(output, checksum(edges));
        for (const Edge& edge : edges) {
            write_u32(output, edge.source);
            write_u32(output, edge.target);
            write_u16(output, edge.type);
        }
        if (!output) {
            throw std::runtime_error("failed writing edge source: " + path);
        }
    }

    static std::vector<Edge> read(const std::string& path,
                                  std::size_t max_vertices,
                                  std::size_t max_edges,
                                  std::size_t& vertex_count) {
        std::ifstream input(path, std::ios::binary);
        if (!input) {
            throw std::runtime_error("cannot open edge source for reading: " + path);
        }
        constexpr char expected_magic[] = {'G', 'E', 'D', 'G', 'E', 'S', '1', '\0'};
        char magic[sizeof(expected_magic)]{};
        input.read(magic, sizeof(magic));
        if (!input || !std::equal(std::begin(magic), std::end(magic),
                                  std::begin(expected_magic))) {
            throw std::runtime_error("invalid durable edge source magic");
        }
        if (read_u32(input) != kFormatVersion) {
            throw std::runtime_error("unsupported durable edge source version");
        }
        const std::uint64_t stored_vertices = read_u64(input);
        const std::uint64_t stored_edges = read_u64(input);
        const std::uint64_t expected_checksum = read_u64(input);
        const std::uint64_t max_vertex_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (stored_vertices == 0 || stored_vertices > max_vertices ||
            stored_vertices > max_vertex_count || stored_edges > max_edges ||
            stored_vertices > std::numeric_limits<std::size_t>::max() ||
            stored_edges > std::numeric_limits<std::size_t>::max()) {
            throw std::runtime_error("durable edge source exceeds configured limits");
        }

        constexpr std::uint64_t header_bytes = 36;
        constexpr std::uint64_t edge_bytes = 10;
        if (stored_edges >
            (std::numeric_limits<std::uint64_t>::max() - header_bytes) /
                edge_bytes) {
            throw std::runtime_error("durable edge source size overflows format limits");
        }
        const std::uint64_t expected_size =
            header_bytes + stored_edges * edge_bytes;
        input.seekg(0, std::ios::end);
        const std::streamoff actual_size = input.tellg();
        if (actual_size < 0 ||
            static_cast<std::uint64_t>(actual_size) != expected_size) {
            throw std::runtime_error("durable edge source has an invalid file size");
        }
        input.seekg(header_bytes, std::ios::beg);

        std::vector<Edge> edges(static_cast<std::size_t>(stored_edges));
        std::uint64_t actual_checksum = kFnvOffset;
        for (Edge& edge : edges) {
            edge.source = read_hashed_u32(input, actual_checksum);
            edge.target = read_hashed_u32(input, actual_checksum);
            edge.type = read_hashed_u16(input, actual_checksum);
            if (edge.source >= stored_vertices || edge.target >= stored_vertices) {
                throw std::runtime_error("durable edge source has an invalid vertex id");
            }
        }
        if (actual_checksum != expected_checksum) {
            throw std::runtime_error("durable edge source checksum mismatch");
        }
        vertex_count = static_cast<std::size_t>(stored_vertices);
        return edges;
    }

private:
    static constexpr std::uint64_t kFnvOffset = 14695981039346656037ULL;
    static constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

    static void validate_edges(std::size_t vertex_count,
                               const std::vector<Edge>& edges) {
        const std::uint64_t max_vertex_count =
            static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
        if (vertex_count == 0 || vertex_count > max_vertex_count) {
            throw std::invalid_argument("invalid durable edge source vertex count");
        }
        for (const Edge& edge : edges) {
            if (edge.source >= vertex_count || edge.target >= vertex_count) {
                throw std::out_of_range(
                    "durable edge source contains an out-of-range vertex");
            }
        }
    }

    static void update_checksum(std::uint64_t& checksum, std::uint8_t byte) {
        checksum ^= byte;
        checksum *= kFnvPrime;
    }

    static std::uint64_t checksum(const std::vector<Edge>& edges) {
        std::uint64_t value = kFnvOffset;
        for (const Edge& edge : edges) {
            for (unsigned int byte = 0; byte < 4; ++byte) {
                update_checksum(value,
                                static_cast<std::uint8_t>(edge.source >> (byte * 8)));
            }
            for (unsigned int byte = 0; byte < 4; ++byte) {
                update_checksum(value,
                                static_cast<std::uint8_t>(edge.target >> (byte * 8)));
            }
            for (unsigned int byte = 0; byte < 2; ++byte) {
                update_checksum(value,
                                static_cast<std::uint8_t>(edge.type >> (byte * 8)));
            }
        }
        return value;
    }

    static void write_u16(std::ostream& output, std::uint16_t value) {
        for (unsigned int byte = 0; byte < 2; ++byte) {
            output.put(static_cast<char>(value >> (byte * 8)));
        }
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
                throw std::runtime_error("truncated durable edge source");
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
                throw std::runtime_error("truncated durable edge source header");
            }
            value |= static_cast<std::uint64_t>(
                         static_cast<unsigned char>(next))
                     << (byte * 8);
        }
        return value;
    }

    static std::uint32_t read_hashed_u32(std::istream& input,
                                         std::uint64_t& checksum_value) {
        const std::uint32_t value = read_u32(input);
        for (unsigned int byte = 0; byte < 4; ++byte) {
            update_checksum(checksum_value,
                            static_cast<std::uint8_t>(value >> (byte * 8)));
        }
        return value;
    }

    static std::uint16_t read_hashed_u16(std::istream& input,
                                         std::uint64_t& checksum_value) {
        std::uint16_t value = 0;
        for (unsigned int byte = 0; byte < 2; ++byte) {
            const int next = input.get();
            if (next == std::char_traits<char>::eof()) {
                throw std::runtime_error("truncated durable edge source payload");
            }
            const auto current = static_cast<std::uint8_t>(next);
            value |= static_cast<std::uint16_t>(current) << (byte * 8);
            update_checksum(checksum_value, current);
        }
        return value;
    }
};

}  // namespace graph_storage

#endif
