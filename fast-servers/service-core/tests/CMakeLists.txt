# Unit tests for service-core. Mirrors what build.zig does under -Dtests; when the two
# disagree, build.zig is right.

# Only this component's include directory, which is the point: a test binary that sees all five
# would never notice a header that does not carry its own dependencies.
add_executable(test_cache test_cache.cpp)
target_include_directories(test_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_cache PRIVATE service_core GTest::gtest)
# The googletest macros do not compile clean under the project's warning flags and are not ours
# to fix, so the test targets do not inherit them.

add_test(NAME service_core.cache COMMAND test_cache)
