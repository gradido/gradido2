# The contract runners: `contracts/test-vectors/<subject>.json`, run against this
# implementation. The TypeScript half of the same files is `packages/contract-tests`, and neither
# half is the authority -- the file is. See vectors.hpp. Mirrors what build.zig does under
# -Dtests; when the two disagree, build.zig is right.

# Not built beside the component it exercises, and the include path is the reason: a runner has
# to read the vector file, which means arnm's header, and a component test that sees a header its
# component does not carry stops proving what service-core/tests exists to prove. So these get
# service-core's include directory, this one, and arnm -- and no other component's.
add_executable(test_jwt_contract test_jwt_contract.cpp)
target_include_directories(test_jwt_contract PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/../../service-core/include
  ${CMAKE_CURRENT_SOURCE_DIR}
  # Populated by gradido-blockchain-core's own CMakeLists; the root file records where.
  ${arnm_SOURCE_DIR}/include
)
# Where the vectors are, compiled in, so the binary finds them without a working directory.
# GRADIDO_CONTRACT_VECTORS_DIR overrides it for one that was moved.
target_compile_definitions(test_jwt_contract PRIVATE
  FS_CONTRACT_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../../../contracts/test-vectors"
)
# As for the unit tests: googletest's macros do not compile clean under the project's warning
# flags and are not ours to fix, so the test targets do not inherit them.
target_link_libraries(test_jwt_contract PRIVATE service_core GTest::gtest)

add_test(NAME contract.jwt COMMAND test_jwt_contract)
