# Unit tests for backend-core. Mirrors what build.zig does under -Dtests; when the two disagree,
# build.zig is right.

# This component's include directory and service-core's, and no more. backend-core is written
# against service-core's headers and links it, so a test of it needs that one; what the rule
# forbids is the other three components', which is what a shared test tree would hand out.
add_executable(test_migrations test_migrations.cpp)
target_include_directories(test_migrations PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include
)
target_link_libraries(test_migrations PRIVATE backend_core service_core GTest::gtest)

# The contract in the binary and the statement splitter. Applying a migration needs a database
# and is not asked here; see the file.
add_test(NAME backend_core.migrations COMMAND test_migrations)

add_executable(test_user test_user.cpp)
target_include_directories(test_user PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include
)
target_link_libraries(test_user PRIVATE backend_core service_core GTest::gtest)

# The half of the user domain that needs no database: the address normalization, the verification
# code's contracted range, and the gradido id ladder.
add_test(NAME backend_core.user COMMAND test_user)
