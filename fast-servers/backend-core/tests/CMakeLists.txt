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

# The half of the user domain that needs no database: the address normalization and the
# verification code's contracted range.
add_test(NAME backend_core.user COMMAND test_user)

add_executable(test_register_account test_register_account.cpp)
target_include_directories(test_register_account PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include
)
target_link_libraries(test_register_account PRIVATE backend_core service_core GTest::gtest)

# The other half: what the unique indexes answer when the two rows are written without asking
# first. Needs a database, and makes its own -- SQLite, on a file it removes again.
add_test(NAME backend_core.register_account COMMAND test_register_account)

add_executable(test_sign_dht_delegation test_sign_dht_delegation.cpp)
target_include_directories(test_sign_dht_delegation PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../include
  ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include
)
target_link_libraries(test_sign_dht_delegation PRIVATE backend_core service_core GTest::gtest)

# The community key, read out of the row the setup wrote, signing this instance's dht identity.
add_test(NAME backend_core.sign_dht_delegation COMMAND test_sign_dht_delegation)
