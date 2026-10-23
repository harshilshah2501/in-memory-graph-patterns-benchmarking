#include "graph_storage.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

using graph_storage::BidirectionalHashGraph;
using graph_storage::CsrCscGraph;
using graph_storage::Edge;
using graph_storage::ForwardHashGraph;
using graph_storage::Vertex;

namespace {

constexpr std::size_t kMaxSupportedEdges = 40000000;
constexpr const char* kCsvSchemaVersion = "v2";

struct Options {
    std::size_t vertices = 50000;
    std::size_t edges = 400000;
    std::size_t queries = 1000;
    int trials = 5;
    int warmups = 0;
    std::size_t hops = 3;
    std::uint32_t seed = 20261021;
    std::string distribution = "uniform";
    std::string output = "text";
    std::string candidate = "all";
};

struct DistributionStats {
    double min = 0;
    double max = 0;
    double mean = 0;
    double stddev = 0;
    double cv_percent = 0;
    double warm_delta_percent = 0;
    double p50 = 0;
    double p95 = 0;
    double p99 = 0;
};

struct ChecksumSamples {
    DistributionStats latency;
    std::uint64_t checksum = 0;
};

struct TraversalStats {
    DistributionStats latency_us;
    std::uint64_t reached_checksum = 0;
};

struct CandidateReport {
    std::string representation;
    DistributionStats build_ms;
    DistributionStats one_hop_forward_ms;
    DistributionStats one_hop_reverse_ms;
    bool has_reverse_metrics = false;
    TraversalStats rca_forward;
    TraversalStats rca_reverse;
    bool has_reverse_rca = false;
    std::size_t retained_index_bytes_est = 0;
    std::size_t edge_count = 0;
    std::size_t vertex_count = 0;
    std::uint64_t one_hop_forward_checksum = 0;
    std::uint64_t one_hop_reverse_checksum = 0;
    bool has_init_maps_phase = false;
    bool has_degree_count_phase = false;
    bool has_prefix_sum_phase = false;
    bool has_reserve_vectors_phase = false;
    bool has_fill_edges_phase = false;
    bool has_sort_ranges_phase = false;
    DistributionStats phase_init_maps_ms;
    DistributionStats phase_degree_count_ms;
    DistributionStats phase_prefix_sum_ms;
    DistributionStats phase_reserve_vectors_ms;
    DistributionStats phase_fill_edges_ms;
    DistributionStats phase_sort_ranges_ms;
    double build_edges_per_sec_p50 = 0;
    double one_hop_forward_queries_per_sec_mean = 0;
    double one_hop_reverse_queries_per_sec_mean = 0;
    double rca_forward_queries_per_sec_mean = 0;
    double rca_reverse_queries_per_sec_mean = 0;
};

struct CorrectnessReport {
    bool one_hop_forward_match = false;
    bool one_hop_reverse_match = false;
    bool rca_forward_match = false;
    bool rca_reverse_match = false;
};

std::size_t parse_size(const std::string& value, const char* option) {
    std::size_t parsed = 0;
    std::size_t consumed = 0;
    try {
        parsed = std::stoull(value, &consumed);
    } catch (const std::exception&) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    if (consumed != value.size()) {
        throw std::invalid_argument(std::string("invalid value for ") + option);
    }
    return parsed;
}

std::size_t percentile_index(std::size_t count, std::size_t percentile) {
    return (count * percentile + 99) / 100 - 1;
}

DistributionStats summarize_samples(std::vector<double> samples) {
    if (samples.empty()) {
        throw std::runtime_error("no timing samples were collected");
    }
    const double cold_sample = samples.front();
    DistributionStats stats;
    double total = 0;
    for (double sample : samples) {
        total += sample;
    }
    stats.mean = total / static_cast<double>(samples.size());
    double variance_total = 0;
    for (double sample : samples) {
        const double delta = sample - stats.mean;
        variance_total += delta * delta;
    }
    stats.stddev = std::sqrt(variance_total / static_cast<double>(samples.size()));
    if (stats.mean > 0) {
        stats.cv_percent = (stats.stddev / stats.mean) * 100.0;
    }
    if (samples.size() > 1) {
        double warm_total = 0;
        for (std::size_t index = 1; index < samples.size(); ++index) {
            warm_total += samples[index];
        }
        const double warm_mean =
            warm_total / static_cast<double>(samples.size() - 1);
        if (cold_sample > 0) {
            stats.warm_delta_percent =
                ((warm_mean - cold_sample) / cold_sample) * 100.0;
        }
    }

    std::sort(samples.begin(), samples.end());
    stats.min = samples.front();
    stats.max = samples.back();
    stats.p50 = samples[(samples.size() - 1) / 2];
    stats.p95 = samples[percentile_index(samples.size(), 95)];
    stats.p99 = samples[percentile_index(samples.size(), 99)];
    return stats;
}

template <typename Function>
DistributionStats measure_ms_samples(int trials, int warmups, Function function) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(trials));
    for (int warmup = 0; warmup < warmups; ++warmup) {
        function();
    }
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        function();
        const auto finish = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }
    return summarize_samples(std::move(samples));
}

template <typename Graph, typename Builder>
DistributionStats measure_build_ms_samples(int trials, int warmups, Builder builder) {
    std::vector<double> samples;
    samples.reserve(static_cast<std::size_t>(trials));
    for (int warmup = 0; warmup < warmups; ++warmup) {
        Graph graph = builder();
        (void)graph;
    }
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        {
            Graph graph = builder();
            (void)graph;
        }
        const auto finish = std::chrono::steady_clock::now();
        samples.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }
    return summarize_samples(std::move(samples));
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string arg = argv[index];
        if (arg == "--help") {
            std::cout
                << "Usage: graph_storage_bench [options]\n"
                << "  --vertices N       Vertex count (default 50000)\n"
                << "  --edges N          Edge count (default 400000, max 40000000)\n"
                << "  --queries N        Query count (default 1000)\n"
                << "  --trials N         Repeated timing trials (default 5)\n"
                << "  --warmups N        Untimed warmup iterations per metric (default 0)\n"
                << "  --hops N           Maximum RCA traversal depth (default 3)\n"
                << "  --seed N           Dataset seed (default 20261021)\n"
                << "  --distribution D   uniform, hub, or clustered\n"
                << "  --output F         text or csv (default text)\n"
                << "  --candidate C      all, forward_hash, bidirectional_hash, or csr_csc\n";
            std::exit(0);
        }

        if (index + 1 >= argc) {
            throw std::invalid_argument("missing value for " + arg);
        }
        const std::string value = argv[++index];
        if (arg == "--vertices") {
            options.vertices = parse_size(value, "--vertices");
        } else if (arg == "--edges") {
            options.edges = parse_size(value, "--edges");
        } else if (arg == "--queries") {
            options.queries = parse_size(value, "--queries");
        } else if (arg == "--trials") {
            const std::size_t parsed = parse_size(value, "--trials");
            if (parsed > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("--trials is too large");
            }
            options.trials = static_cast<int>(parsed);
        } else if (arg == "--warmups") {
            const std::size_t parsed = parse_size(value, "--warmups");
            if (parsed > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
                throw std::invalid_argument("--warmups is too large");
            }
            options.warmups = static_cast<int>(parsed);
        } else if (arg == "--hops") {
            options.hops = parse_size(value, "--hops");
        } else if (arg == "--seed") {
            const std::size_t parsed = parse_size(value, "--seed");
            if (parsed > std::numeric_limits<std::uint32_t>::max()) {
                throw std::invalid_argument("--seed is too large");
            }
            options.seed = static_cast<std::uint32_t>(parsed);
        } else if (arg == "--distribution") {
            options.distribution = value;
        } else if (arg == "--output") {
            options.output = value;
        } else if (arg == "--candidate") {
            options.candidate = value;
        } else {
            throw std::invalid_argument("unknown option: " + arg);
        }
    }

    const std::uint64_t max_vertex_count =
        static_cast<std::uint64_t>(std::numeric_limits<Vertex>::max()) + 1;
    if (options.vertices == 0 || options.vertices > max_vertex_count) {
        throw std::invalid_argument("--vertices must fit the Vertex id range");
    }
    if (options.edges == 0 || options.queries == 0 || options.trials <= 0 ||
        options.hops == 0) {
        throw std::invalid_argument(
            "--edges, --queries, --trials, and --hops must be positive");
    }
    if (options.warmups > std::numeric_limits<int>::max() - options.trials) {
        throw std::invalid_argument("--warmups plus --trials is too large");
    }
    if (options.edges > kMaxSupportedEdges) {
        throw std::invalid_argument(
            "--edges exceeds the configured maximum of 40000000");
    }
    if (options.distribution != "uniform" && options.distribution != "hub" &&
        options.distribution != "clustered") {
        throw std::invalid_argument(
            "--distribution must be uniform, hub, or clustered");
    }
    if (options.output != "text" && options.output != "csv") {
        throw std::invalid_argument("--output must be text or csv");
    }
    if (options.candidate != "all" && options.candidate != "forward_hash" &&
        options.candidate != "bidirectional_hash" &&
        options.candidate != "csr_csc") {
        throw std::invalid_argument(
            "--candidate must be all, forward_hash, bidirectional_hash, or csr_csc");
    }
    return options;
}

std::vector<Edge> make_edges(const Options& options) {
    std::mt19937 random(options.seed);
    std::uniform_int_distribution<Vertex> any_vertex(
        0, static_cast<Vertex>(options.vertices - 1));
    const std::size_t hot_count =
        std::max<std::size_t>(1, options.vertices / 100);
    std::uniform_int_distribution<Vertex> hot_vertex(
        0, static_cast<Vertex>(hot_count - 1));
    std::uniform_int_distribution<std::uint16_t> edge_type(0, 3);
    std::vector<Edge> edges;
    edges.reserve(options.edges);
    const std::size_t cluster_size =
        std::max<std::size_t>(1, options.vertices / 16);
    std::uniform_int_distribution<std::size_t> cluster(
        0, (options.vertices - 1) / cluster_size);

    for (std::size_t index = 0; index < options.edges; ++index) {
        Vertex source = any_vertex(random);
        Vertex target = any_vertex(random);
        if (options.distribution == "hub") {
            if (index % 10 < 8) {
                source = hot_vertex(random);
            }
            if (index % 10 < 8) {
                target = hot_vertex(random);
            }
        } else if (options.distribution == "clustered") {
            const std::size_t source_cluster = cluster(random);
            const std::size_t target_cluster =
                (index % 10 < 8) ? source_cluster : cluster(random);
            const std::size_t source_id =
                source_cluster * cluster_size +
                (random() %
                 std::min(cluster_size,
                          options.vertices - source_cluster * cluster_size));
            const std::size_t target_start = target_cluster * cluster_size;
            const std::size_t target_id =
                target_start +
                (random() %
                 std::min(cluster_size, options.vertices - target_start));
            source = static_cast<Vertex>(source_id);
            target = static_cast<Vertex>(target_id);
        }
        edges.push_back(Edge{source, target, edge_type(random)});
    }
    return edges;
}

std::vector<Vertex> make_queries(const Options& options) {
    std::mt19937 random(options.seed + 1);
    std::uniform_int_distribution<Vertex> vertex(
        0, static_cast<Vertex>(options.vertices - 1));
    const std::size_t hot_count =
        std::max<std::size_t>(1, options.vertices / 100);
    std::uniform_int_distribution<Vertex> hot_vertex(
        0, static_cast<Vertex>(hot_count - 1));
    std::vector<Vertex> queries;
    queries.reserve(options.queries);
    for (std::size_t index = 0; index < options.queries; ++index) {
        if (options.distribution == "hub" && index % 2 == 0) {
            queries.push_back(hot_vertex(random));
        } else {
            queries.push_back(vertex(random));
        }
    }
    return queries;
}

struct TraversalResult {
    std::size_t reached = 0;
    std::uint64_t checksum = 0;
};

template <typename Graph, typename NeighborsFor>
TraversalResult traverse(const Graph& graph,
                         Vertex seed,
                         std::size_t max_hops,
                         NeighborsFor neighbors_for,
                         std::vector<std::uint32_t>& visited,
                         std::vector<Vertex>& queue,
                         std::uint32_t mark) {
    queue.clear();
    queue.push_back(seed);
    visited[seed] = mark;

    TraversalResult result{1, static_cast<std::uint64_t>(seed) + 1};
    std::size_t level_begin = 0;
    std::size_t level_end = 1;
    for (std::size_t depth = 0; depth < max_hops && level_begin < level_end;
         ++depth) {
        const std::size_t next_level_begin = level_end;
        while (level_begin < next_level_begin) {
            const Vertex current = queue[level_begin++];
            const auto& neighbors = neighbors_for(graph, current);
            for (const auto& neighbor : neighbors) {
                if (visited[neighbor.vertex] != mark) {
                    visited[neighbor.vertex] = mark;
                    queue.push_back(neighbor.vertex);
                    ++result.reached;
                    result.checksum +=
                        static_cast<std::uint64_t>(neighbor.vertex) + 1;
                }
            }
        }
        level_end = queue.size();
    }
    return result;
}

template <typename Graph, typename NeighborsFor>
TraversalStats benchmark_traversal(const Graph& graph,
                                   const std::vector<Vertex>& seeds,
                                   std::size_t max_hops,
                                   NeighborsFor neighbors_for,
                                   int trials,
                                   int warmups) {
    std::vector<double> latencies_us;
    latencies_us.reserve(seeds.size() * static_cast<std::size_t>(trials));
    std::vector<std::uint32_t> visited(graph.vertex_count(), 0);
    std::vector<Vertex> queue;
    queue.reserve(graph.vertex_count());
    std::uint32_t mark = 0;
    std::uint64_t aggregate_checksum = 0;

    const auto run_seed_pass = [&](bool record) {
        for (Vertex seed : seeds) {
            if (mark == std::numeric_limits<std::uint32_t>::max()) {
                std::fill(visited.begin(), visited.end(), 0);
                mark = 0;
            }
            ++mark;
            const auto start =
                record ? std::chrono::steady_clock::now()
                       : std::chrono::steady_clock::time_point{};
            const TraversalResult result =
                traverse(graph, seed, max_hops, neighbors_for, visited, queue,
                         mark);
            if (record) {
                const auto finish = std::chrono::steady_clock::now();
                latencies_us.push_back(std::chrono::duration<double, std::micro>(
                                           finish - start)
                                           .count());
                aggregate_checksum += result.checksum + result.reached;
            }
        }
    };
    for (int warmup = 0; warmup < warmups; ++warmup) {
        run_seed_pass(false);
    }
    for (int trial = 0; trial < trials; ++trial) {
        run_seed_pass(true);
    }

    return TraversalStats{summarize_samples(std::move(latencies_us)),
                          aggregate_checksum};
}

template <typename Graph>
std::uint64_t scan_out_neighbors(const Graph& graph,
                                 const std::vector<Vertex>& queries) {
    std::uint64_t checksum = 0;
    for (Vertex vertex : queries) {
        for (const auto& neighbor : graph.out_neighbors(vertex)) {
            checksum += static_cast<std::uint64_t>(neighbor.vertex) +
                        neighbor.type + 1;
        }
    }
    return checksum;
}

template <typename Graph>
std::uint64_t scan_in_neighbors(const Graph& graph,
                                const std::vector<Vertex>& queries) {
    std::uint64_t checksum = 0;
    for (Vertex vertex : queries) {
        for (const auto& neighbor : graph.in_neighbors(vertex)) {
            checksum += static_cast<std::uint64_t>(neighbor.vertex) +
                        neighbor.type + 1;
        }
    }
    return checksum;
}

template <typename Function>
ChecksumSamples benchmark_checksum_samples(int trials,
                                           int warmups,
                                           Function function) {
    std::vector<double> latencies_ms;
    latencies_ms.reserve(static_cast<std::size_t>(trials));
    std::uint64_t checksum = 0;
    for (int warmup = 0; warmup < warmups; ++warmup) {
        function();
    }
    for (int trial = 0; trial < trials; ++trial) {
        const auto start = std::chrono::steady_clock::now();
        checksum = function();
        const auto finish = std::chrono::steady_clock::now();
        latencies_ms.push_back(
            std::chrono::duration<double, std::milli>(finish - start).count());
    }
    return ChecksumSamples{summarize_samples(std::move(latencies_ms)), checksum};
}

double bytes_per_edge(std::size_t bytes, std::size_t edges) {
    return edges == 0 ? 0 : static_cast<double>(bytes) / static_cast<double>(edges);
}

double bytes_per_vertex(std::size_t bytes, std::size_t vertices) {
    return vertices == 0
               ? 0
               : static_cast<double>(bytes) / static_cast<double>(vertices);
}

double edges_per_second_from_ms(std::size_t edges, double milliseconds) {
    if (milliseconds <= 0) {
        return 0;
    }
    return static_cast<double>(edges) / (milliseconds / 1000.0);
}

double queries_per_second_from_ms(double milliseconds_per_query) {
    if (milliseconds_per_query <= 0) {
        return 0;
    }
    return 1000.0 / milliseconds_per_query;
}

double queries_per_second_from_us(double microseconds_per_query) {
    if (microseconds_per_query <= 0) {
        return 0;
    }
    return 1000000.0 / microseconds_per_query;
}

void profile_forward_hash_phases(CandidateReport& report,
                                 std::size_t vertex_count,
                                 const std::vector<Edge>& edges,
                                 int trials,
                                 int warmups) {
    std::vector<double> init_samples;
    std::vector<double> fill_samples;
    init_samples.reserve(static_cast<std::size_t>(trials));
    fill_samples.reserve(static_cast<std::size_t>(trials));
    std::uint64_t guard = 0;

    for (int trial = 0; trial < warmups + trials; ++trial) {
        const bool record = trial >= warmups;
        std::unordered_map<Vertex, std::vector<graph_storage::Neighbor>> outgoing;

        const auto init_start = std::chrono::steady_clock::now();
        outgoing.reserve(vertex_count);
        for (std::size_t id = 0; id < vertex_count; ++id) {
            outgoing.emplace(static_cast<Vertex>(id),
                             std::vector<graph_storage::Neighbor>{});
        }
        const auto init_end = std::chrono::steady_clock::now();
        if (record) {
            init_samples.push_back(
                std::chrono::duration<double, std::milli>(init_end - init_start)
                    .count());
        }

        const auto fill_start = std::chrono::steady_clock::now();
        for (const Edge& edge : edges) {
            if (edge.source >= vertex_count || edge.target >= vertex_count) {
                throw std::out_of_range("edge or query vertex is out of range");
            }
            outgoing.at(edge.source).push_back(
                graph_storage::Neighbor{edge.target, edge.type});
        }
        const auto fill_end = std::chrono::steady_clock::now();
        if (record) {
            fill_samples.push_back(
                std::chrono::duration<double, std::milli>(fill_end - fill_start)
                    .count());
        }
        guard += outgoing.size();
    }

    if (guard == 0) {
        throw std::runtime_error("phase profiling guard unexpectedly zero");
    }
    report.has_init_maps_phase = true;
    report.has_fill_edges_phase = true;
    report.phase_init_maps_ms = summarize_samples(std::move(init_samples));
    report.phase_fill_edges_ms = summarize_samples(std::move(fill_samples));
}

void profile_bidirectional_hash_phases(CandidateReport& report,
                                       std::size_t vertex_count,
                                       const std::vector<Edge>& edges,
                                       int trials,
                                       int warmups) {
    std::vector<double> init_samples;
    std::vector<double> degree_samples;
    std::vector<double> reserve_samples;
    std::vector<double> fill_samples;
    init_samples.reserve(static_cast<std::size_t>(trials));
    degree_samples.reserve(static_cast<std::size_t>(trials));
    reserve_samples.reserve(static_cast<std::size_t>(trials));
    fill_samples.reserve(static_cast<std::size_t>(trials));
    std::uint64_t guard = 0;

    for (int trial = 0; trial < warmups + trials; ++trial) {
        const bool record = trial >= warmups;
        std::unordered_map<Vertex, std::vector<graph_storage::Neighbor>> outgoing;
        std::unordered_map<Vertex, std::vector<graph_storage::Neighbor>> incoming;

        const auto init_start = std::chrono::steady_clock::now();
        outgoing.reserve(vertex_count);
        incoming.reserve(vertex_count);
        for (std::size_t id = 0; id < vertex_count; ++id) {
            const Vertex vertex = static_cast<Vertex>(id);
            outgoing.emplace(vertex, std::vector<graph_storage::Neighbor>{});
            incoming.emplace(vertex, std::vector<graph_storage::Neighbor>{});
        }
        const auto init_end = std::chrono::steady_clock::now();
        if (record) {
            init_samples.push_back(
                std::chrono::duration<double, std::milli>(init_end - init_start)
                    .count());
        }

        const auto degree_start = std::chrono::steady_clock::now();
        std::vector<std::size_t> out_degree(vertex_count, 0);
        std::vector<std::size_t> in_degree(vertex_count, 0);
        for (const Edge& edge : edges) {
            if (edge.source >= vertex_count || edge.target >= vertex_count) {
                throw std::out_of_range("edge or query vertex is out of range");
            }
            ++out_degree[edge.source];
            ++in_degree[edge.target];
        }
        const auto degree_end = std::chrono::steady_clock::now();
        if (record) {
            degree_samples.push_back(
                std::chrono::duration<double, std::milli>(degree_end - degree_start)
                    .count());
        }

        const auto reserve_start = std::chrono::steady_clock::now();
        for (std::size_t id = 0; id < vertex_count; ++id) {
            outgoing.at(static_cast<Vertex>(id)).reserve(out_degree[id]);
            incoming.at(static_cast<Vertex>(id)).reserve(in_degree[id]);
        }
        const auto reserve_end = std::chrono::steady_clock::now();
        if (record) {
            reserve_samples.push_back(
                std::chrono::duration<double, std::milli>(reserve_end - reserve_start)
                    .count());
        }

        const auto fill_start = std::chrono::steady_clock::now();
        for (const Edge& edge : edges) {
            outgoing.at(edge.source).push_back(
                graph_storage::Neighbor{edge.target, edge.type});
            incoming.at(edge.target).push_back(
                graph_storage::Neighbor{edge.source, edge.type});
        }
        const auto fill_end = std::chrono::steady_clock::now();
        if (record) {
            fill_samples.push_back(
                std::chrono::duration<double, std::milli>(fill_end - fill_start)
                    .count());
        }
        guard += outgoing.size() + incoming.size();
    }

    if (guard == 0) {
        throw std::runtime_error("phase profiling guard unexpectedly zero");
    }
    report.has_init_maps_phase = true;
    report.has_degree_count_phase = true;
    report.has_reserve_vectors_phase = true;
    report.has_fill_edges_phase = true;
    report.phase_init_maps_ms = summarize_samples(std::move(init_samples));
    report.phase_degree_count_ms = summarize_samples(std::move(degree_samples));
    report.phase_reserve_vectors_ms = summarize_samples(std::move(reserve_samples));
    report.phase_fill_edges_ms = summarize_samples(std::move(fill_samples));
}

void profile_csr_csc_phases(CandidateReport& report,
                            std::size_t vertex_count,
                            const std::vector<Edge>& edges,
                            int trials,
                            int warmups) {
    std::vector<double> degree_samples;
    std::vector<double> prefix_samples;
    std::vector<double> fill_samples;
    std::vector<double> sort_samples;
    degree_samples.reserve(static_cast<std::size_t>(trials));
    prefix_samples.reserve(static_cast<std::size_t>(trials));
    fill_samples.reserve(static_cast<std::size_t>(trials));
    sort_samples.reserve(static_cast<std::size_t>(trials));
    std::uint64_t guard = 0;

    for (int trial = 0; trial < warmups + trials; ++trial) {
        const bool record = trial >= warmups;
        std::vector<std::size_t> out_offsets(vertex_count + 1, 0);
        std::vector<std::size_t> in_offsets(vertex_count + 1, 0);

        const auto degree_start = std::chrono::steady_clock::now();
        for (const Edge& edge : edges) {
            if (edge.source >= vertex_count || edge.target >= vertex_count) {
                throw std::out_of_range("edge or query vertex is out of range");
            }
            ++out_offsets[static_cast<std::size_t>(edge.source) + 1];
            ++in_offsets[static_cast<std::size_t>(edge.target) + 1];
        }
        const auto degree_end = std::chrono::steady_clock::now();
        if (record) {
            degree_samples.push_back(
                std::chrono::duration<double, std::milli>(degree_end - degree_start)
                    .count());
        }

        const auto prefix_start = std::chrono::steady_clock::now();
        for (std::size_t index = 1; index < out_offsets.size(); ++index) {
            out_offsets[index] += out_offsets[index - 1];
            in_offsets[index] += in_offsets[index - 1];
        }
        const auto prefix_end = std::chrono::steady_clock::now();
        if (record) {
            prefix_samples.push_back(
                std::chrono::duration<double, std::milli>(prefix_end - prefix_start)
                    .count());
        }

        const auto fill_start = std::chrono::steady_clock::now();
        std::vector<graph_storage::Neighbor> out_neighbors(edges.size());
        std::vector<graph_storage::Neighbor> in_neighbors(edges.size());
        std::vector<std::size_t> out_cursor(out_offsets.begin(),
                                            out_offsets.end() - 1);
        std::vector<std::size_t> in_cursor(in_offsets.begin(),
                                           in_offsets.end() - 1);
        for (const Edge& edge : edges) {
            out_neighbors[out_cursor[edge.source]++] =
                graph_storage::Neighbor{edge.target, edge.type};
            in_neighbors[in_cursor[edge.target]++] =
                graph_storage::Neighbor{edge.source, edge.type};
        }
        const auto fill_end = std::chrono::steady_clock::now();
        if (record) {
            fill_samples.push_back(
                std::chrono::duration<double, std::milli>(fill_end - fill_start)
                    .count());
        }

        const auto sort_start = std::chrono::steady_clock::now();
        const auto neighbor_order = [](const graph_storage::Neighbor& left,
                                       const graph_storage::Neighbor& right) {
            if (left.type != right.type) {
                return left.type < right.type;
            }
            return left.vertex < right.vertex;
        };
        for (std::size_t vertex = 0; vertex < vertex_count; ++vertex) {
            std::sort(out_neighbors.begin() + out_offsets[vertex],
                      out_neighbors.begin() + out_offsets[vertex + 1],
                      neighbor_order);
            std::sort(in_neighbors.begin() + in_offsets[vertex],
                      in_neighbors.begin() + in_offsets[vertex + 1],
                      neighbor_order);
        }
        const auto sort_end = std::chrono::steady_clock::now();
        if (record) {
            sort_samples.push_back(
                std::chrono::duration<double, std::milli>(sort_end - sort_start)
                    .count());
        }
        guard += out_neighbors.size() + in_neighbors.size();
    }

    if (guard == 0) {
        throw std::runtime_error("phase profiling guard unexpectedly zero");
    }
    report.has_degree_count_phase = true;
    report.has_prefix_sum_phase = true;
    report.has_fill_edges_phase = true;
    report.has_sort_ranges_phase = true;
    report.phase_degree_count_ms = summarize_samples(std::move(degree_samples));
    report.phase_prefix_sum_ms = summarize_samples(std::move(prefix_samples));
    report.phase_fill_edges_ms = summarize_samples(std::move(fill_samples));
    report.phase_sort_ranges_ms = summarize_samples(std::move(sort_samples));
}

void print_distribution(const std::string& label,
                        const DistributionStats& stats,
                        const std::string& unit) {
    std::cout << "  " << label << " [" << unit << "] min=" << stats.min
              << " p50=" << stats.p50 << " p95=" << stats.p95
              << " p99=" << stats.p99 << " mean=" << stats.mean
              << " stddev=" << stats.stddev << " cv%=" << stats.cv_percent
              << " warm_delta%=" << stats.warm_delta_percent
              << " max=" << stats.max << '\n';
}

void print_candidate_report(const CandidateReport& report) {
    std::cout << report.representation << '\n';
    print_distribution("build", report.build_ms, "ms");
    print_distribution("one-hop forward", report.one_hop_forward_ms, "ms");
    if (report.has_reverse_metrics) {
        print_distribution("one-hop reverse", report.one_hop_reverse_ms, "ms");
    }
    print_distribution("RCA forward", report.rca_forward.latency_us, "us");
    if (report.has_reverse_rca) {
        print_distribution("RCA reverse", report.rca_reverse.latency_us, "us");
    }
    std::cout << "  throughput build_p50 [edges/s] "
              << report.build_edges_per_sec_p50 << '\n';
    std::cout << "  throughput one-hop forward [queries/s, mean] "
              << report.one_hop_forward_queries_per_sec_mean << '\n';
    if (report.has_reverse_metrics) {
        std::cout << "  throughput one-hop reverse [queries/s, mean] "
                  << report.one_hop_reverse_queries_per_sec_mean << '\n';
    }
    std::cout << "  throughput RCA forward [queries/s, mean] "
              << report.rca_forward_queries_per_sec_mean << '\n';
    if (report.has_reverse_rca) {
        std::cout << "  throughput RCA reverse [queries/s, mean] "
                  << report.rca_reverse_queries_per_sec_mean << '\n';
    }
    std::cout << "  retained index estimate [bytes] "
              << report.retained_index_bytes_est << " (" << std::fixed
              << std::setprecision(3)
              << bytes_per_edge(report.retained_index_bytes_est, report.edge_count)
              << " B/edge, "
              << bytes_per_vertex(report.retained_index_bytes_est,
                                  report.vertex_count)
              << " B/vertex)\n";
    std::cout.unsetf(std::ios::floatfield);
    std::cout << "  one-hop forward checksum " << report.one_hop_forward_checksum
              << '\n';
    if (report.has_reverse_metrics) {
        std::cout << "  one-hop reverse checksum "
                  << report.one_hop_reverse_checksum << '\n';
    }
    std::cout << "  RCA forward checksum " << report.rca_forward.reached_checksum
              << '\n';
    if (report.has_reverse_rca) {
        std::cout << "  RCA reverse checksum " << report.rca_reverse.reached_checksum
                  << '\n';
    }
    std::cout << "  build phase timing p50 [ms]\n";
    if (report.has_init_maps_phase) {
        std::cout << "    init_maps: " << report.phase_init_maps_ms.p50 << '\n';
    }
    if (report.has_degree_count_phase) {
        std::cout << "    degree_count: " << report.phase_degree_count_ms.p50
                  << '\n';
    }
    if (report.has_prefix_sum_phase) {
        std::cout << "    prefix_sum: " << report.phase_prefix_sum_ms.p50 << '\n';
    }
    if (report.has_reserve_vectors_phase) {
        std::cout << "    reserve_vectors: " << report.phase_reserve_vectors_ms.p50
                  << '\n';
    }
    if (report.has_fill_edges_phase) {
        std::cout << "    fill_edges: " << report.phase_fill_edges_ms.p50 << '\n';
    }
    if (report.has_sort_ranges_phase) {
        std::cout << "    sort_ranges: " << report.phase_sort_ranges_ms.p50 << '\n';
    }
}

void print_text_report(const Options& options,
                       const CandidateReport& forward_hash,
                       const CandidateReport& bidirectional_hash,
                       const CandidateReport& csr,
                       const CorrectnessReport& correctness) {
    std::cout << "Benchmark workload: edges=" << options.edges
              << ", vertices=" << options.vertices
              << ", distribution=" << options.distribution
              << ", seed=" << options.seed << ", queries=" << options.queries
              << ", hops=" << options.hops << ", trials=" << options.trials
              << ", warmups=" << options.warmups
              << '\n';
    std::cout << "Configured edge cap: " << kMaxSupportedEdges << "\n\n";
    std::cout << "CSV schema version: " << kCsvSchemaVersion << "\n\n";

    print_candidate_report(forward_hash);
    std::cout << '\n';
    print_candidate_report(bidirectional_hash);
    std::cout << '\n';
    print_candidate_report(csr);
    std::cout << '\n';

    std::cout << "Correctness gates\n";
    std::cout << "  one-hop forward parity: "
              << (correctness.one_hop_forward_match ? "PASS" : "FAIL") << '\n';
    std::cout << "  one-hop reverse parity: "
              << (correctness.one_hop_reverse_match ? "PASS" : "FAIL") << '\n';
    std::cout << "  RCA forward parity: "
              << (correctness.rca_forward_match ? "PASS" : "FAIL") << '\n';
    std::cout << "  RCA reverse parity: "
              << (correctness.rca_reverse_match ? "PASS" : "FAIL") << '\n';
    std::cout << "Peak RSS must be measured externally (for example: "
                 "/usr/bin/time -v ./build/graph_storage_bench ...).\n";
}

void print_csv_header() {
    std::cout
        << "schema_version,representation,edges,vertices,distribution,seed,queries,hops,trials,"
        << "build_min_ms,build_p50_ms,build_p95_ms,build_p99_ms,build_mean_ms,"
        << "build_stddev_ms,build_cv_percent,build_warm_delta_percent,build_max_ms,"
        << "one_hop_forward_min_ms,one_hop_forward_p50_ms,"
        << "one_hop_forward_p95_ms,one_hop_forward_p99_ms,"
        << "one_hop_forward_mean_ms,one_hop_forward_stddev_ms,"
        << "one_hop_forward_cv_percent,one_hop_forward_warm_delta_percent,"
        << "one_hop_forward_max_ms,"
        << "one_hop_reverse_min_ms,one_hop_reverse_p50_ms,"
        << "one_hop_reverse_p95_ms,one_hop_reverse_p99_ms,"
        << "one_hop_reverse_mean_ms,one_hop_reverse_stddev_ms,"
        << "one_hop_reverse_cv_percent,one_hop_reverse_warm_delta_percent,"
        << "one_hop_reverse_max_ms,"
        << "rca_forward_min_us,rca_forward_p50_us,rca_forward_p95_us,"
        << "rca_forward_p99_us,rca_forward_mean_us,rca_forward_stddev_us,"
        << "rca_forward_cv_percent,rca_forward_warm_delta_percent,rca_forward_max_us,"
        << "rca_reverse_min_us,rca_reverse_p50_us,rca_reverse_p95_us,"
        << "rca_reverse_p99_us,rca_reverse_mean_us,rca_reverse_stddev_us,"
        << "rca_reverse_cv_percent,rca_reverse_warm_delta_percent,rca_reverse_max_us,"
        << "build_edges_per_sec_p50,one_hop_forward_qps_mean,"
        << "one_hop_reverse_qps_mean,rca_forward_qps_mean,rca_reverse_qps_mean,"
        << "phase_init_maps_p50_ms,phase_degree_count_p50_ms,"
        << "phase_prefix_sum_p50_ms,phase_reserve_vectors_p50_ms,"
        << "phase_fill_edges_p50_ms,phase_sort_ranges_p50_ms,"
        << "retained_index_bytes_est,retained_bytes_per_edge_est,"
        << "retained_bytes_per_vertex_est,one_hop_forward_checksum,"
        << "one_hop_reverse_checksum,rca_forward_checksum,rca_reverse_checksum,warmups\n";
}

void print_csv_row(const Options& options, const CandidateReport& report) {
    auto write_stats = [](const DistributionStats& stats) {
        std::cout << stats.min << ',' << stats.p50 << ',' << stats.p95 << ','
                  << stats.p99 << ',' << stats.mean << ',' << stats.stddev << ','
                  << stats.cv_percent << ',' << stats.warm_delta_percent << ','
                  << stats.max;
    };
    auto write_empty_stats = []() { std::cout << ",,,,,,,,"; };

    std::cout << kCsvSchemaVersion << ',' << report.representation << ','
              << options.edges << ','
              << options.vertices << ',' << options.distribution << ','
              << options.seed << ',' << options.queries << ',' << options.hops
              << ',' << options.trials << ',';
    write_stats(report.build_ms);
    std::cout << ',';
    write_stats(report.one_hop_forward_ms);
    std::cout << ',';
    if (report.has_reverse_metrics) {
        write_stats(report.one_hop_reverse_ms);
    } else {
        write_empty_stats();
    }
    std::cout << ',';
    write_stats(report.rca_forward.latency_us);
    std::cout << ',';
    if (report.has_reverse_rca) {
        write_stats(report.rca_reverse.latency_us);
    } else {
        write_empty_stats();
    }
    std::cout << ',' << report.build_edges_per_sec_p50 << ','
              << report.one_hop_forward_queries_per_sec_mean << ',';
    if (report.has_reverse_metrics) {
        std::cout << report.one_hop_reverse_queries_per_sec_mean;
    }
    std::cout << ',' << report.rca_forward_queries_per_sec_mean << ',';
    if (report.has_reverse_rca) {
        std::cout << report.rca_reverse_queries_per_sec_mean;
    }
    std::cout << ',';
    if (report.has_init_maps_phase) {
        std::cout << report.phase_init_maps_ms.p50;
    }
    std::cout << ',';
    if (report.has_degree_count_phase) {
        std::cout << report.phase_degree_count_ms.p50;
    }
    std::cout << ',';
    if (report.has_prefix_sum_phase) {
        std::cout << report.phase_prefix_sum_ms.p50;
    }
    std::cout << ',';
    if (report.has_reserve_vectors_phase) {
        std::cout << report.phase_reserve_vectors_ms.p50;
    }
    std::cout << ',';
    if (report.has_fill_edges_phase) {
        std::cout << report.phase_fill_edges_ms.p50;
    }
    std::cout << ',';
    if (report.has_sort_ranges_phase) {
        std::cout << report.phase_sort_ranges_ms.p50;
    }
    std::cout << ',' << report.retained_index_bytes_est << ','
              << bytes_per_edge(report.retained_index_bytes_est, report.edge_count)
              << ','
              << bytes_per_vertex(report.retained_index_bytes_est,
                                  report.vertex_count)
              << ',' << report.one_hop_forward_checksum << ',';
    if (report.has_reverse_metrics) {
        std::cout << report.one_hop_reverse_checksum;
    }
    std::cout << ',' << report.rca_forward.reached_checksum << ',';
    if (report.has_reverse_rca) {
        std::cout << report.rca_reverse.reached_checksum;
    }
    std::cout << ',' << options.warmups << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        const std::vector<Edge> edges = make_edges(options);
        const std::vector<Vertex> queries = make_queries(options);
        const bool run_forward_hash =
            options.candidate == "all" || options.candidate == "forward_hash";
        const bool run_bidirectional_hash =
            options.candidate == "all" || options.candidate == "bidirectional_hash";
        const bool run_csr = options.candidate == "all" || options.candidate == "csr_csc";

        CandidateReport forward_hash;
        if (run_forward_hash) {
            forward_hash.representation = "forward_hash";
            forward_hash.vertex_count = options.vertices;
            forward_hash.edge_count = options.edges;
            forward_hash.build_ms =
                measure_build_ms_samples<ForwardHashGraph>(
                    options.trials, options.warmups, [&] {
                    return ForwardHashGraph::build(options.vertices, edges);
                });
            profile_forward_hash_phases(forward_hash, options.vertices, edges,
                                        options.trials, options.warmups);
            {
                const ForwardHashGraph graph =
                    ForwardHashGraph::build(options.vertices, edges);
                forward_hash.retained_index_bytes_est =
                    graph.retained_index_bytes_estimate();
                const ChecksumSamples one_hop =
                    benchmark_checksum_samples(options.trials, options.warmups, [&] {
                        return scan_out_neighbors(graph, queries);
                    });
                forward_hash.one_hop_forward_ms = one_hop.latency;
                forward_hash.one_hop_forward_checksum = one_hop.checksum;
                forward_hash.rca_forward = benchmark_traversal(
                    graph, queries, options.hops,
                    [](const ForwardHashGraph& current_graph, Vertex vertex) {
                        return current_graph.out_neighbors(vertex);
                    },
                    options.trials, options.warmups);
            }
            forward_hash.build_edges_per_sec_p50 =
                edges_per_second_from_ms(options.edges, forward_hash.build_ms.p50);
            forward_hash.one_hop_forward_queries_per_sec_mean =
                queries_per_second_from_ms(forward_hash.one_hop_forward_ms.mean);
            forward_hash.rca_forward_queries_per_sec_mean =
                queries_per_second_from_us(forward_hash.rca_forward.latency_us.mean);
        }

        CandidateReport bidirectional_hash;
        if (run_bidirectional_hash) {
            bidirectional_hash.representation = "bidirectional_hash";
            bidirectional_hash.vertex_count = options.vertices;
            bidirectional_hash.edge_count = options.edges;
            bidirectional_hash.has_reverse_metrics = true;
            bidirectional_hash.has_reverse_rca = true;
            bidirectional_hash.build_ms =
                measure_build_ms_samples<BidirectionalHashGraph>(
                    options.trials, options.warmups, [&] {
                        return BidirectionalHashGraph::build(options.vertices, edges);
                    });
            profile_bidirectional_hash_phases(bidirectional_hash, options.vertices,
                                              edges, options.trials, options.warmups);
            {
                const BidirectionalHashGraph graph =
                    BidirectionalHashGraph::build(options.vertices, edges);
                bidirectional_hash.retained_index_bytes_est =
                    graph.retained_index_bytes_estimate();
                const ChecksumSamples one_hop_out =
                    benchmark_checksum_samples(options.trials, options.warmups, [&] {
                        return scan_out_neighbors(graph, queries);
                    });
                bidirectional_hash.one_hop_forward_ms = one_hop_out.latency;
                bidirectional_hash.one_hop_forward_checksum = one_hop_out.checksum;
                const ChecksumSamples one_hop_in =
                    benchmark_checksum_samples(options.trials, options.warmups, [&] {
                        return scan_in_neighbors(graph, queries);
                    });
                bidirectional_hash.one_hop_reverse_ms = one_hop_in.latency;
                bidirectional_hash.one_hop_reverse_checksum = one_hop_in.checksum;
                bidirectional_hash.rca_forward = benchmark_traversal(
                    graph, queries, options.hops,
                    [](const BidirectionalHashGraph& current_graph, Vertex vertex) {
                        return current_graph.out_neighbors(vertex);
                    },
                    options.trials, options.warmups);
                bidirectional_hash.rca_reverse = benchmark_traversal(
                    graph, queries, options.hops,
                    [](const BidirectionalHashGraph& current_graph, Vertex vertex) {
                        return current_graph.in_neighbors(vertex);
                    },
                    options.trials, options.warmups);
            }
            bidirectional_hash.build_edges_per_sec_p50 = edges_per_second_from_ms(
                options.edges, bidirectional_hash.build_ms.p50);
            bidirectional_hash.one_hop_forward_queries_per_sec_mean =
                queries_per_second_from_ms(
                    bidirectional_hash.one_hop_forward_ms.mean);
            bidirectional_hash.one_hop_reverse_queries_per_sec_mean =
                queries_per_second_from_ms(
                    bidirectional_hash.one_hop_reverse_ms.mean);
            bidirectional_hash.rca_forward_queries_per_sec_mean =
                queries_per_second_from_us(
                    bidirectional_hash.rca_forward.latency_us.mean);
            bidirectional_hash.rca_reverse_queries_per_sec_mean =
                queries_per_second_from_us(
                    bidirectional_hash.rca_reverse.latency_us.mean);
        }

        CandidateReport csr;
        if (run_csr) {
            csr.representation = "csr_csc";
            csr.vertex_count = options.vertices;
            csr.edge_count = options.edges;
            csr.has_reverse_metrics = true;
            csr.has_reverse_rca = true;
            csr.build_ms = measure_build_ms_samples<CsrCscGraph>(
                options.trials, options.warmups, [&] {
                    return CsrCscGraph::build(options.vertices, edges);
                });
            profile_csr_csc_phases(csr, options.vertices, edges, options.trials,
                                   options.warmups);
            {
                const CsrCscGraph graph =
                    CsrCscGraph::build(options.vertices, edges);
                csr.retained_index_bytes_est = graph.retained_index_bytes_estimate();
                const ChecksumSamples one_hop_out =
                    benchmark_checksum_samples(options.trials, options.warmups, [&] {
                        return scan_out_neighbors(graph, queries);
                    });
                csr.one_hop_forward_ms = one_hop_out.latency;
                csr.one_hop_forward_checksum = one_hop_out.checksum;
                const ChecksumSamples one_hop_in =
                    benchmark_checksum_samples(options.trials, options.warmups, [&] {
                        return scan_in_neighbors(graph, queries);
                    });
                csr.one_hop_reverse_ms = one_hop_in.latency;
                csr.one_hop_reverse_checksum = one_hop_in.checksum;
                csr.rca_forward = benchmark_traversal(
                    graph, queries, options.hops,
                    [](const CsrCscGraph& current_graph, Vertex vertex) {
                        return current_graph.out_neighbors(vertex);
                    },
                    options.trials, options.warmups);
                csr.rca_reverse = benchmark_traversal(
                    graph, queries, options.hops,
                    [](const CsrCscGraph& current_graph, Vertex vertex) {
                        return current_graph.in_neighbors(vertex);
                    },
                    options.trials, options.warmups);
            }
            csr.build_edges_per_sec_p50 =
                edges_per_second_from_ms(options.edges, csr.build_ms.p50);
            csr.one_hop_forward_queries_per_sec_mean =
                queries_per_second_from_ms(csr.one_hop_forward_ms.mean);
            csr.one_hop_reverse_queries_per_sec_mean =
                queries_per_second_from_ms(csr.one_hop_reverse_ms.mean);
            csr.rca_forward_queries_per_sec_mean =
                queries_per_second_from_us(csr.rca_forward.latency_us.mean);
            csr.rca_reverse_queries_per_sec_mean =
                queries_per_second_from_us(csr.rca_reverse.latency_us.mean);
        }

        CorrectnessReport correctness;
        const bool has_all_candidates =
            run_forward_hash && run_bidirectional_hash && run_csr;
        if (has_all_candidates) {
            correctness.one_hop_forward_match =
                forward_hash.one_hop_forward_checksum ==
                    bidirectional_hash.one_hop_forward_checksum &&
                bidirectional_hash.one_hop_forward_checksum ==
                    csr.one_hop_forward_checksum;
            correctness.one_hop_reverse_match =
                bidirectional_hash.one_hop_reverse_checksum ==
                csr.one_hop_reverse_checksum;
            correctness.rca_forward_match =
                forward_hash.rca_forward.reached_checksum ==
                    bidirectional_hash.rca_forward.reached_checksum &&
                bidirectional_hash.rca_forward.reached_checksum ==
                    csr.rca_forward.reached_checksum;
            correctness.rca_reverse_match =
                bidirectional_hash.rca_reverse.reached_checksum ==
                csr.rca_reverse.reached_checksum;

            if (!correctness.one_hop_forward_match ||
                !correctness.one_hop_reverse_match ||
                !correctness.rca_forward_match ||
                !correctness.rca_reverse_match) {
                throw std::runtime_error("candidate graph results do not match");
            }
        } else {
            correctness.one_hop_forward_match = true;
            correctness.one_hop_reverse_match = true;
            correctness.rca_forward_match = true;
            correctness.rca_reverse_match = true;
        }

        if (options.output == "text") {
            if (has_all_candidates) {
                print_text_report(options, forward_hash, bidirectional_hash, csr,
                                  correctness);
            } else {
                std::cout << "Benchmark workload: edges=" << options.edges
                          << ", vertices=" << options.vertices
                          << ", distribution=" << options.distribution
                          << ", seed=" << options.seed
                          << ", queries=" << options.queries
                          << ", hops=" << options.hops
                          << ", trials=" << options.trials
                          << ", warmups=" << options.warmups
                          << ", candidate=" << options.candidate << '\n';
                std::cout << "Configured edge cap: " << kMaxSupportedEdges << '\n';
                std::cout << "CSV schema version: " << kCsvSchemaVersion << "\n\n";
                if (run_forward_hash) {
                    print_candidate_report(forward_hash);
                }
                if (run_bidirectional_hash) {
                    if (run_forward_hash) {
                        std::cout << '\n';
                    }
                    print_candidate_report(bidirectional_hash);
                }
                if (run_csr) {
                    if (run_forward_hash || run_bidirectional_hash) {
                        std::cout << '\n';
                    }
                    print_candidate_report(csr);
                }
                std::cout << "\nCross-candidate correctness gates were skipped "
                             "because --candidate selected a subset.\n";
                std::cout << "Peak RSS must be measured externally (for example: "
                             "/usr/bin/time -v ./build/graph_storage_bench ...).\n";
            }
        } else {
            print_csv_header();
            if (run_forward_hash) {
                print_csv_row(options, forward_hash);
            }
            if (run_bidirectional_hash) {
                print_csv_row(options, bidirectional_hash);
            }
            if (run_csr) {
                print_csv_row(options, csr);
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "Benchmark configuration or execution failed: "
                  << error.what() << '\n';
        return 2;
    }
}
