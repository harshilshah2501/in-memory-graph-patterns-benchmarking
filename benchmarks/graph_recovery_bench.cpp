#include "graph_recovery.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <vector>
#include <unistd.h>

using graph_storage::CsrCscGraph;
using graph_storage::DurableEdgeSource;
using graph_storage::Edge;
using graph_storage::ForwardHashGraph;
using graph_storage::BidirectionalHashGraph;
using graph_storage::Vertex;

namespace {

constexpr std::size_t kMaxEdges = 40000000;

struct Options {
    std::size_t vertices = 0;
    std::size_t edges = 0;
    int trials = 3;
    int warmups = 1;
    std::uint32_t seed = 20261021;
    std::string distribution = "uniform";
    std::string mode = "prepare";
    std::string candidate = "all";
    std::string cache_state = "warm-process-restart";
    std::string output_dir;
};

std::size_t parse_size(const std::string& value, const char* name) {
    std::size_t consumed = 0;
    unsigned long long parsed = 0;
    try {
        parsed = std::stoull(value, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid value for ") + name);
    }
    if (consumed != value.size() ||
        parsed > std::numeric_limits<std::size_t>::max()) {
        throw std::invalid_argument(std::string("invalid value for ") + name);
    }
    return static_cast<std::size_t>(parsed);
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string name = argv[index];
        if (name == "--help") {
            std::cout
                << "Usage: graph_recovery_bench --mode prepare|source-rebuild|snapshot-load "
                   "--vertices N --edges N --output-dir PATH [options]\n"
                << "  --trials N         Timed recovery trials (default 3)\n"
                << "  --warmups N        Untimed recovery iterations (default 1)\n"
                << "  --seed N           Dataset seed (default 20261021)\n"
                << "  --distribution D   uniform, hub, or clustered\n"
                << "  --candidate C      all, forward_hash, bidirectional_hash, csr_csc\n"
                << "  --cache-state S    warm-process-restart or best-effort-file-advice\n";
            std::exit(0);
        }
        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + name);
        }
        const std::string value = argv[++index];
        if (name == "--vertices") {
            options.vertices = parse_size(value, "--vertices");
        } else if (name == "--edges") {
            options.edges = parse_size(value, "--edges");
        } else if (name == "--trials") {
            const std::size_t parsed = parse_size(value, "--trials");
            if (parsed == 0 || parsed > static_cast<std::size_t>(
                                             std::numeric_limits<int>::max())) {
                throw std::invalid_argument("--trials must be positive and fit int");
            }
            options.trials = static_cast<int>(parsed);
        } else if (name == "--warmups") {
            const std::size_t parsed = parse_size(value, "--warmups");
            if (parsed > static_cast<std::size_t>(
                             std::numeric_limits<int>::max() - options.trials)) {
                throw std::invalid_argument("--warmups is too large");
            }
            options.warmups = static_cast<int>(parsed);
        } else if (name == "--seed") {
            const std::size_t parsed = parse_size(value, "--seed");
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--seed exceeds uint32");
            }
            options.seed = static_cast<std::uint32_t>(parsed);
        } else if (name == "--distribution") {
            options.distribution = value;
        } else if (name == "--mode") {
            options.mode = value;
        } else if (name == "--candidate") {
            options.candidate = value;
        } else if (name == "--cache-state") {
            options.cache_state = value;
        } else if (name == "--output-dir") {
            options.output_dir = value;
        } else {
            throw std::invalid_argument("unknown option: " + name);
        }
    }
    const std::uint64_t max_vertices =
        static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
    if (options.vertices == 0 || options.vertices > max_vertices ||
        options.edges == 0 || options.edges > kMaxEdges ||
        options.output_dir.empty()) {
        throw std::invalid_argument(
            "--vertices, --edges, and --output-dir are required; edges max at 40000000");
    }
    if (options.distribution != "uniform" && options.distribution != "hub" &&
        options.distribution != "clustered") {
        throw std::invalid_argument(
            "--distribution must be uniform, hub, or clustered");
    }
    if (options.mode != "prepare" && options.mode != "source-rebuild" &&
        options.mode != "snapshot-load") {
        throw std::invalid_argument(
            "--mode must be prepare, source-rebuild, or snapshot-load");
    }
    if (options.candidate != "all" && options.candidate != "forward_hash" &&
        options.candidate != "bidirectional_hash" &&
        options.candidate != "csr_csc") {
        throw std::invalid_argument(
            "--candidate must be all, forward_hash, bidirectional_hash, or csr_csc");
    }
    if (options.mode != "prepare" && options.candidate == "all") {
        throw std::invalid_argument(
            "--candidate must select one representation outside prepare mode");
    }
    if (options.cache_state != "warm-process-restart" &&
        options.cache_state != "best-effort-file-advice") {
        throw std::invalid_argument(
            "--cache-state must be warm-process-restart or best-effort-file-advice");
    }
    return options;
}

std::vector<Edge> make_edges(const Options& options) {
    std::mt19937 random(options.seed);
    std::uniform_int_distribution<Vertex> any_vertex(
        0, static_cast<Vertex>(options.vertices - 1));
    const std::size_t hot_count = std::max<std::size_t>(1, options.vertices / 100);
    std::uniform_int_distribution<Vertex> hot_vertex(
        0, static_cast<Vertex>(hot_count - 1));
    std::uniform_int_distribution<std::uint16_t> edge_type(0, 3);
    const std::size_t cluster_size =
        std::max<std::size_t>(1, options.vertices / 16);
    std::uniform_int_distribution<std::size_t> cluster(
        0, (options.vertices - 1) / cluster_size);
    std::vector<Edge> edges;
    edges.reserve(options.edges);
    for (std::size_t index = 0; index < options.edges; ++index) {
        Vertex source = any_vertex(random);
        Vertex target = any_vertex(random);
        if (options.distribution == "hub" && index % 10 < 8) {
            source = hot_vertex(random);
            target = hot_vertex(random);
        } else if (options.distribution == "clustered") {
            const std::size_t source_cluster = cluster(random);
            const std::size_t target_cluster =
                index % 10 < 8 ? source_cluster : cluster(random);
            const std::size_t source_start = source_cluster * cluster_size;
            const std::size_t target_start = target_cluster * cluster_size;
            source = static_cast<Vertex>(
                source_start + random() %
                                   std::min(cluster_size,
                                            options.vertices - source_start));
            target = static_cast<Vertex>(
                target_start + random() %
                                   std::min(cluster_size,
                                            options.vertices - target_start));
        }
        edges.push_back(Edge{source, target, edge_type(random)});
    }
    return edges;
}

std::uint64_t graph_checksum(const CsrCscGraph& graph) {
    std::uint64_t checksum = 14695981039346656037ULL;
    const auto add = [&checksum](std::uint64_t value, unsigned int bytes) {
        for (unsigned int byte = 0; byte < bytes; ++byte) {
            checksum ^= static_cast<std::uint8_t>(value >> (byte * 8));
            checksum *= 1099511628211ULL;
        }
    };
    add(graph.vertex_count(), 8);
    add(graph.edge_count(), 8);
    for (std::size_t id = 0; id < graph.vertex_count(); ++id) {
        const auto out = graph.out_neighbors(static_cast<Vertex>(id));
        add(out.size(), 8);
        for (const auto& neighbor : out) {
            add(neighbor.vertex, 4);
            add(neighbor.type, 2);
        }
        const auto in = graph.in_neighbors(static_cast<Vertex>(id));
        add(in.size(), 8);
        for (const auto& neighbor : in) {
            add(neighbor.vertex, 4);
            add(neighbor.type, 2);
        }
    }
    return checksum;
}

std::uint64_t graph_checksum(const ForwardHashGraph& graph) {
    std::uint64_t checksum = 14695981039346656037ULL;
    const auto add = [&checksum](std::uint64_t value, unsigned int bytes) {
        for (unsigned int byte = 0; byte < bytes; ++byte) {
            checksum ^= static_cast<std::uint8_t>(value >> (byte * 8));
            checksum *= 1099511628211ULL;
        }
    };
    add(graph.vertex_count(), 8);
    add(graph.edge_count(), 8);
    for (std::size_t id = 0; id < graph.vertex_count(); ++id) {
        const auto& out = graph.out_neighbors(static_cast<Vertex>(id));
        add(out.size(), 8);
        for (const auto& neighbor : out) {
            add(neighbor.vertex, 4);
            add(neighbor.type, 2);
        }
    }
    return checksum;
}

std::uint64_t graph_checksum(const BidirectionalHashGraph& graph) {
    std::uint64_t checksum = 14695981039346656037ULL;
    const auto add = [&checksum](std::uint64_t value, unsigned int bytes) {
        for (unsigned int byte = 0; byte < bytes; ++byte) {
            checksum ^= static_cast<std::uint8_t>(value >> (byte * 8));
            checksum *= 1099511628211ULL;
        }
    };
    add(graph.vertex_count(), 8);
    add(graph.edge_count(), 8);
    for (std::size_t id = 0; id < graph.vertex_count(); ++id) {
        const auto add_neighbors = [&add](const auto& neighbors) {
            add(neighbors.size(), 8);
            for (const auto& neighbor : neighbors) {
                add(neighbor.vertex, 4);
                add(neighbor.type, 2);
            }
        };
        add_neighbors(graph.out_neighbors(static_cast<Vertex>(id)));
        add_neighbors(graph.in_neighbors(static_cast<Vertex>(id)));
    }
    return checksum;
}

std::string snapshot_path(const Options& options, const std::string& candidate) {
    return (std::filesystem::path(options.output_dir) /
            (candidate + ".snapshot"))
        .string();
}

void advise_drop_file_cache(const std::string& path) {
    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot open file for cache advice: " + path);
    }
    const int result = posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
    const int close_result = close(descriptor);
    if (result != 0) {
        throw std::system_error(result, std::generic_category(),
                                "per-file cache eviction advice failed: " + path);
    }
    if (close_result != 0) {
        throw std::system_error(errno, std::generic_category(),
                                "cannot close cache-advice descriptor: " + path);
    }
}

void apply_cache_state(const Options& options, const std::string& input_path) {
    if (options.cache_state == "best-effort-file-advice") {
        advise_drop_file_cache(input_path);
    }
}

struct Stats {
    double min;
    double p50;
    double p95;
    double mean;
    double max;
};

Stats summarize(std::vector<double> values) {
    if (values.empty()) {
        throw std::runtime_error("cannot summarize empty timing samples");
    }
    std::sort(values.begin(), values.end());
    double total = 0;
    for (double value : values) {
        total += value;
    }
    return Stats{values.front(), values[(values.size() - 1) / 2],
                 values[(values.size() * 95 + 99) / 100 - 1],
                 total / static_cast<double>(values.size()), values.back()};
}

template <typename Function>
Stats measure(int trials, int warmups, Function function) {
    for (int warmup = 0; warmup < warmups; ++warmup) {
        function();
    }
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(trials));
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        function();
        const auto finish = std::chrono::steady_clock::now();
        times.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }
    return summarize(std::move(times));
}

template <typename BeforeOperation, typename Function>
Stats measure_ready_time(int trials,
                         int warmups,
                         BeforeOperation before_operation,
                         Function function) {
    for (int warmup = 0; warmup < warmups; ++warmup) {
        before_operation();
        auto ready_graph = function();
        (void)ready_graph;
    }
    std::vector<double> times;
    times.reserve(static_cast<std::size_t>(trials));
    for (int trial = 0; trial < trials; ++trial) {
        before_operation();
        const auto start = std::chrono::steady_clock::now();
        {
            auto ready_graph = function();
            const auto finish = std::chrono::steady_clock::now();
            times.push_back(
                std::chrono::duration<double, std::milli>(finish - start).count());
            (void)ready_graph;
        }
    }
    return summarize(std::move(times));
}

void write_stats(std::ofstream& output, const Stats& stats) {
    output << stats.min << ',' << stats.p50 << ',' << stats.p95 << ','
           << stats.mean << ',' << stats.max;
}

template <typename Graph>
double prepare_candidate(const std::vector<Edge>& edges,
                         const Options& options,
                         const std::string& candidate) {
    Graph graph = Graph::build(options.vertices, edges);
    const auto snapshot_start = std::chrono::steady_clock::now();
    graph.save_snapshot(snapshot_path(options, candidate));
    const auto snapshot_finish = std::chrono::steady_clock::now();
    std::ofstream reference(
        std::filesystem::path(options.output_dir) /
            ("reference_checksum_" + candidate + ".txt"),
        std::ios::trunc);
    if (!reference) {
        throw std::runtime_error("cannot write reference graph checksum");
    }
    reference << graph_checksum(graph) << '\n';
    if (!reference) {
        throw std::runtime_error("failed writing reference graph checksum");
    }
    return std::chrono::duration<double, std::milli>(
               snapshot_finish - snapshot_start)
        .count();
}

template <typename Graph>
void run_candidate(const Options& options,
                   const std::string& edge_source_path) {
    const std::string candidate = options.candidate;
    const std::string input_path = options.mode == "source-rebuild"
        ? edge_source_path
        : snapshot_path(options, candidate);
    std::ifstream reference(
        std::filesystem::path(options.output_dir) /
            ("reference_checksum_" + candidate + ".txt"));
    std::uint64_t expected_checksum = 0;
    if (!(reference >> expected_checksum)) {
        throw std::runtime_error("missing or invalid candidate reference checksum");
    }

    const auto before_operation = [&] {
        apply_cache_state(options, input_path);
    };
    const auto load = [&] {
        if (options.mode == "snapshot-load") {
            return std::make_unique<Graph>(
                Graph::load_snapshot(input_path, options.vertices, options.edges));
        }
        std::size_t loaded_vertices = 0;
        auto loaded_edges = DurableEdgeSource::read(
            edge_source_path, options.vertices, options.edges, loaded_vertices);
        if (loaded_edges.size() != options.edges ||
            loaded_vertices != options.vertices) {
            throw std::runtime_error("durable edge source dimensions mismatch");
        }
        return std::make_unique<Graph>(
            Graph::build(loaded_vertices, loaded_edges));
    };

    const Stats ready_time = measure_ready_time(
        options.trials, options.warmups, before_operation, load);
    auto restored = load();
    const std::uint64_t restored_checksum = graph_checksum(*restored);
    if (restored_checksum != expected_checksum) {
        throw std::runtime_error(
            options.mode + " failed graph equivalence validation");
    }

    const std::string results_path =
        (std::filesystem::path(options.output_dir) /
         (options.mode + "_" + candidate + "_" + options.cache_state +
          "_results.csv"))
            .string();
    std::ofstream output(results_path, std::ios::trunc);
    if (!output) {
        throw std::runtime_error("cannot create recovery result CSV");
    }
    output << std::fixed << std::setprecision(6)
           << "schema_version,operation,candidate,cache_state,edges,vertices,"
              "distribution,seed,trials,warmups,ready_min_ms,ready_p50_ms,"
              "ready_p95_ms,ready_mean_ms,ready_max_ms,expected_checksum,"
              "restored_checksum,equivalence\n"
           << "recovery-v2," << options.mode << ',' << candidate << ','
           << options.cache_state << ',' << options.edges << ','
           << options.vertices << ',' << options.distribution << ','
           << options.seed << ',' << options.trials << ',' << options.warmups
           << ',';
    write_stats(output, ready_time);
    output << ',' << expected_checksum << ',' << restored_checksum << ",PASS\n";
    if (!output) {
        throw std::runtime_error("failed writing recovery result CSV");
    }
    std::cout << options.mode << " " << candidate << " "
              << options.cache_state << " ready-time p50: "
              << ready_time.p50 << " ms\n";
    std::cout << "Graph equivalence: PASS\n";
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        std::filesystem::create_directories(options.output_dir);
        const std::string edge_source_path =
            (std::filesystem::path(options.output_dir) / "edges.bin").string();
        if (options.mode == "prepare") {
            const std::vector<Edge> edges = make_edges(options);
            const auto source_start = std::chrono::steady_clock::now();
            DurableEdgeSource::write(edge_source_path, options.vertices, edges);
            const auto source_finish = std::chrono::steady_clock::now();
            const std::vector<std::pair<std::string, double>> candidate_writes = {
                {"forward_hash",
                 prepare_candidate<ForwardHashGraph>(edges, options,
                                                     "forward_hash")},
                {"bidirectional_hash",
                 prepare_candidate<BidirectionalHashGraph>(
                     edges, options, "bidirectional_hash")},
                {"csr_csc",
                 prepare_candidate<CsrCscGraph>(edges, options, "csr_csc")}};
            std::ofstream prepare_results(
                std::filesystem::path(options.output_dir) / "prepare_results.csv",
                std::ios::trunc);
            if (!prepare_results) {
                throw std::runtime_error("cannot create preparation result CSV");
            }
            prepare_results << std::fixed << std::setprecision(6)
                            << "schema_version,edges,vertices,distribution,seed,"
                               "source_bytes,source_write_ms,candidate,"
                               "snapshot_bytes,snapshot_write_ms\n";
            for (const auto& [candidate, snapshot_write_ms] : candidate_writes) {
                prepare_results << "recovery-v2," << options.edges << ','
                                << options.vertices << ','
                                << options.distribution << ',' << options.seed
                                << ','
                                << std::filesystem::file_size(edge_source_path)
                                << ','
                                << std::chrono::duration<double, std::milli>(
                                       source_finish - source_start)
                                       .count()
                                << ',' << candidate << ','
                                << std::filesystem::file_size(
                                       snapshot_path(options, candidate))
                                << ',' << snapshot_write_ms << '\n';
            }
            if (!prepare_results) {
                throw std::runtime_error("failed writing preparation result CSV");
            }
            std::cout << "Durable source and all candidate snapshots prepared.\n";
            std::cout << "Durable source bytes: "
                      << std::filesystem::file_size(edge_source_path) << '\n';
        } else if (options.candidate == "forward_hash") {
            run_candidate<ForwardHashGraph>(options, edge_source_path);
        } else if (options.candidate == "bidirectional_hash") {
            run_candidate<BidirectionalHashGraph>(options, edge_source_path);
        } else {
            run_candidate<CsrCscGraph>(options, edge_source_path);
        }
    } catch (const std::exception& error) {
        std::cerr << "Recovery benchmark failed: " << error.what() << '\n';
        return 2;
    }
}
