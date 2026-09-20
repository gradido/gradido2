# Unit tests for the dht-node role. Mirrors what build.zig does under -Dtests; when the two
# disagree, build.zig is right.

# The module as this build links it; added only where one is linked, the stub has nothing to test.
add_executable(test_libp2p_ffi test_libp2p_ffi.cpp)
target_include_directories(test_libp2p_ffi PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../include)
target_link_libraries(test_libp2p_ffi PRIVATE GTest::gtest)
fs_link_libp2p_ffi(test_libp2p_ffi)

# Two nodes on loopback, a call from one to the other and its answer.
add_test(NAME dht_node.libp2p_ffi COMMAND test_libp2p_ffi)

# Peer ids in text, compiled in: the module is private to the role.
add_executable(test_peer_id test_peer_id.cpp ${CMAKE_CURRENT_SOURCE_DIR}/../src/peer_id.c)
target_include_directories(test_peer_id PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../src)
target_link_libraries(test_peer_id PRIVATE GTest::gtest)
add_test(NAME dht_node.peer_id COMMAND test_peer_id)
