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

# static_sites.c is compiled in for the same reason field_rules.c is: it is the leaf half of the
# static web server -- which file a path means -- and linking the role to reach it would drag the
# HTTP backend into a unit test. static_routes.c is the half that answers, and is not here.
add_executable(test_static_sites test_static_sites.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/static_sites.c)
target_include_directories(test_static_sites PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include)
target_link_libraries(test_static_sites PRIVATE GTest::gtest)

# Which site a path belongs to and which file of it -- the two decisions made before anything is
# answered, against a table this test writes rather than the one the build generates.
add_test(NAME backend.static_sites COMMAND test_static_sites)
