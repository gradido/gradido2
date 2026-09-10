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

add_executable(test_sql test_sql.cpp)
target_include_directories(test_sql PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_sql PRIVATE service_core GTest::gtest)

# Statements: every value type through both databases, the refusals, the cursor. SQLite on a
# file it removes again; PostgreSQL when SC_DB_TEST_PG_HOST is set.
add_test(NAME service_core.sql COMMAND test_sql)

add_executable(test_db_exec test_db_exec.cpp)
target_include_directories(test_db_exec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_db_exec PRIVATE service_core GTest::gtest)

# The executor: where a unit runs, how its transaction ends, a full queue and a late unit. One
# test waits out SC_DB_QUEUE_WAIT_MS on purpose. PostgreSQL when SC_DB_TEST_PG_HOST is set.
add_test(NAME service_core.db_exec COMMAND test_db_exec)

add_executable(test_topology test_topology.cpp)
target_include_directories(test_topology PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_topology PRIVATE service_core GTest::gtest)

# Cache groups: the kernel's CPU lists, the CPUs a process may use, proportional spreading.
add_test(NAME service_core.topology COMMAND test_topology)

add_executable(test_env_file test_env_file.cpp)
target_include_directories(test_env_file PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_env_file PRIVATE service_core GTest::gtest)

# Writes and reads one file in the working directory. What it decides is the agreement with
# dotenv, which packages/backend/src/setup/envFile.test.ts measures from the other side.
add_test(NAME service_core.env_file COMMAND test_env_file)

add_executable(test_jwt test_jwt.cpp)
target_include_directories(test_jwt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_jwt PRIVATE service_core GTest::gtest)

# jwt.c from the outside, and only what a contract cannot express -- null arguments, the buffers
# at their bounds, the sign-then-verify round trip. Which payloads are refused is shared
# behaviour and lives in contracts/test-vectors/jwt.json, run by ../../tests/contract.
add_test(NAME service_core.jwt COMMAND test_jwt)

add_executable(test_secret test_secret.cpp)
target_include_directories(test_secret PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_secret PRIVATE service_core GTest::gtest)

# Where a secret comes from and in which order -- contracts/secrets.json -- plus the `.env`
# reader that fills what nobody exported. The reference path asserts the same cases in
# packages/service-core/src/config/secret.test.ts.
add_test(NAME service_core.secret COMMAND test_secret)

add_executable(test_log test_log.cpp)
target_include_directories(test_log PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_log PRIVATE service_core GTest::gtest)

# The logger from the outside: the contracted line byte for byte, the escaping, the level
# filter, and the three things only a running ring can be wrong about -- backpressure that
# drops, arenas that do not come back, lines torn between eight threads.
add_test(NAME service_core.log COMMAND test_log)

# The one test that sees a component's src/: the two calls it holds together are not on
# service-core's surface, and it needs arnm's header because they take an arnm_byte_buffer.
# arnm_SOURCE_DIR is where the core populated it -- see the top of ../../CMakeLists.txt.
add_executable(test_log_pretty test_log_pretty.cpp)
target_include_directories(test_log_pretty PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../src/log
  ${arnm_SOURCE_DIR}/include)
target_link_libraries(test_log_pretty PRIVATE service_core GTest::gtest)
# Built with assertions on whatever the build type is: arnm's unchecked appends check their
# preconditions only where NDEBUG is off, and those checks are half of what this test is for.
target_compile_options(test_log_pretty PRIVATE $<$<CXX_COMPILER_ID:MSVC>:/UNDEBUG>
                                               $<$<NOT:$<CXX_COMPILER_ID:MSVC>>:-UNDEBUG>)

# The measured length of a pretty line against the length its encoder writes, over every level,
# category, value kind, count, number boundary and clock reading.
add_test(NAME service_core.log_pretty COMMAND test_log_pretty)
