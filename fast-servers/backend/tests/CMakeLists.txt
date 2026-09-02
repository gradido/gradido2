# Unit tests for the backend role. Mirrors what build.zig does under -Dtests; when the two
# disagree, build.zig is right.

# field_rules.c is compiled in rather than linked through the component: it is a leaf that
# reaches no header outside its own directory, and linking the role would drag the HTTP backend
# into a unit test for the sake of three text rules.
add_executable(test_field_rules test_field_rules.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/field_rules.c)
target_include_directories(test_field_rules PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_field_rules PRIVATE GTest::gtest)

# The valibot rules of packages/shared/src/schemas, as the route applies them -- the JavaScript
# string semantics above all, which is where reproducing a library goes wrong first.
add_test(NAME backend.field_rules COMMAND test_field_rules)
