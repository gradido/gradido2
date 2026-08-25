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

add_executable(test_mail test_mail.cpp)
target_include_directories(test_mail PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_mail PRIVATE service_core GTest::gtest)

# Skips its one live test unless SC_MAIL_TEST_URL names a listening SMTP server; everything
# else it checks is refusal, and refusal needs no network.
add_test(NAME service_core.mail COMMAND test_mail)

add_executable(test_db test_db.cpp)
target_include_directories(test_db PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_db PRIVATE service_core GTest::gtest)

# Skips whatever this build has no driver for, and skips its one live test unless
# SC_DB_TEST_PG_HOST names a reachable PostgreSQL. Everything else it checks is configuration,
# refusal, and SQLite, which is a file.
add_test(NAME service_core.db COMMAND test_db)
