# Hot-path performance benchmarks (docs/adr/0011-benchmark-tooling.md). Only
# built when BUILD_BENCHMARKS=ON (see top-level CMakeLists.txt).

function(indexed_add_benchmark name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE indexed_core benchmark::benchmark)
    indexed_enable_warnings(${name})
endfunction()

indexed_add_benchmark(bench_SearchEngine)
indexed_add_benchmark(bench_WalkScanner)
