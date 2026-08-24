# Mirrors the -Dbenchmarks branch of build.zig. Linked against the same core and the same
# libsodium the server is, because what a JWT costs depends on which SHA-256 is underneath.
#
# Build type matters more here than anywhere else: a Debug build measures Debug.
#   cmake -B build -DFS_ENABLE_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release

add_executable(bench_jwt bench_jwt.c)
target_include_directories(bench_jwt PRIVATE ${FS_INCLUDE_DIRS} ${arnm_SOURCE_DIR}/include)
target_compile_options(bench_jwt PRIVATE ${FS_WARNING_FLAGS})
target_link_libraries(bench_jwt PRIVATE service_core)
