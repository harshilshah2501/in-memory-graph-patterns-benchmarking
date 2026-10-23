CXX ?= c++
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
CPPFLAGS ?= -Iinclude

.PHONY: all test benchmark benchmark-rss benchmark-schema-check benchmark-matrix benchmark-recovery clean

all: test

test: build/graph_storage_test
	./build/graph_storage_test

benchmark: build/graph_storage_bench
	./build/graph_storage_bench

benchmark-rss: build/graph_storage_bench
	./scripts/measure_peak_rss.sh

benchmark-schema-check: build/graph_storage_bench
	./scripts/validate_benchmark_csv_schema.sh

benchmark-matrix: build/graph_storage_bench
	./scripts/run_benchmark_matrix.sh

benchmark-recovery: build/graph_recovery_bench
	./scripts/run_graph_recovery_bench.sh

build/graph_storage_test: tests/graph_storage_test.cpp include/graph_storage.hpp include/graph_recovery.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/graph_storage_bench: benchmarks/graph_storage_bench.cpp include/graph_storage.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

build/graph_recovery_bench: benchmarks/graph_recovery_bench.cpp include/graph_recovery.hpp include/graph_storage.hpp
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@

clean:
	rm -rf build
