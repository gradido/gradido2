cmake_minimum_required(VERSION 3.18.2)
cmake_policy(VERSION 3.18.2)

# CMP0135 makes FetchContent stamp extracted archives with the extraction time instead of the
# timestamps inside the tarball. It only exists from CMake 3.24 on, and setting an unknown policy
# is a hard error -- so it is asked for rather than assumed.
if(POLICY CMP0135)
  cmake_policy(SET CMP0135 NEW)
endif()

# build.zig is the master build of this project -- it defines the targets, the options and the
# matrix that gets verified. This file exists for the one case zig cannot serve: the MSVC ABI on
# Windows, whose SDK zig does not ship. Everything here mirrors build.zig rather than leading it;
# when the two disagree, build.zig is right. See ../AGENTS.md.
#
#   cmake -B build
#   cmake --build build --config Release
#
# Everything it needs is fetched. The one thing it does not build is h2o -- see FS_ENABLE_H2O.
#
# The dependencies are fetched, not looked for. It is the second build of a project whose first
# one downloads everything it needs, and a Windows developer who has to install libuv by hand
# before the tree compiles will conclude the tree does not compile.
#
# On Linux this file works too and is the better of the two builds for one purpose: it prints the
# -Wall -Wextra findings. zig hides C compiler warnings when a step succeeds and turns them into
# errors when one fails, so a green `zig build` says nothing about them either way.

project(fast_servers C)

# CMake only learned to pass /std:c11 to MSVC in 3.20; older versions accept these lines and
# quietly hand MSVC its default C mode instead. MSVC itself needs VS 2019 16.8 for C11, which
# ships with a newer CMake anyway -- but if a Windows build fails on C11 syntax, check the CMake
# version before the source.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(FetchContent)

# Off by default, and here is where this file does NOT mirror build.zig: `zig build` fetches h2o
# and compiles libh2o-evloop itself, this one cannot. Asking pkg-config for a libh2o-evloop that
# almost nobody has installed would make `cmake -B build` fail out of the box on the very
# platform the project targets, which is worse than building the backend that needs nothing.
# Turn it on where the library is actually installed.
option(FS_ENABLE_H2O "Build the h2o HTTP backend instead of the libuv+picohttpparser fallback. Needs a libh2o-evloop installed and visible to pkg-config; unavailable on Windows" OFF)
# The SQLite driver, built from sqlite.org's amalgamation the way build.zig builds it -- one C
# file, fetched, so this needs nothing installed and defaults on as it does there.
option(FS_ENABLE_SQLITE "Build the SQLite driver" ON)
# PostgreSQL, and the second place this file does not mirror build.zig. `zig build` compiles
# libpq out of a pinned postgres checkout and needs nothing installed; this build asks the host
# for one, the way it asks for h2o and curl -- and the host this file exists for is a Windows
# developer's, where libpq is not normally there. Defaulting it on would fail `cmake -B build`
# out of the box for a driver that build.zig provides anyway. Turn it on where libpq is
# installed; the binary then carries both, exactly as the zig build's does.
option(FS_ENABLE_POSTGRES "Build the PostgreSQL driver. Needs libpq and its headers installed and visible to find_package(PostgreSQL)" OFF)
option(FS_ENABLE_TESTS "Build the googletest unit tests and the integration probe" OFF)
option(FS_ENABLE_BENCHMARKS "Build the bench_* binaries" OFF)
option(FS_ENABLE_SANITIZERS "Build with AddressSanitizer, plus UndefinedBehaviorSanitizer on non-MSVC toolchains (MSVC ships no UBSan)" OFF)
option(FS_ENABLE_THREAD_SANITIZER "Build with ThreadSanitizer (mutually exclusive with FS_ENABLE_SANITIZERS)" OFF)

if(WIN32 AND FS_ENABLE_H2O)
  message(FATAL_ERROR "FS_ENABLE_H2O does not build for Windows: h2o is a posix event loop. Leave it off and the binary serves over libuv and picohttpparser instead.")
endif()

# Set before any target is declared, so every component is instrumented the same way -- mixing an
# instrumented library with an uninstrumented binary produces false reports.
if(FS_ENABLE_SANITIZERS AND FS_ENABLE_THREAD_SANITIZER)
  message(FATAL_ERROR "FS_ENABLE_SANITIZERS and FS_ENABLE_THREAD_SANITIZER cannot be combined; ThreadSanitizer does not run together with AddressSanitizer")
endif()

if(FS_ENABLE_SANITIZERS)
  if(MSVC)
    # MSVC ships AddressSanitizer only, there is no UndefinedBehaviorSanitizer counterpart. This
    # is also the one sanitizer build.zig cannot offer: zig does not ship the asan runtime, which
    # is why AGENTS.md section 4 needs both builds to satisfy its list.
    message(STATUS "Sanitizers enabled: address")
    add_compile_options(/fsanitize=address /Zi)
  else()
    message(STATUS "Sanitizers enabled: address, undefined")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

if(FS_ENABLE_THREAD_SANITIZER)
  if(MSVC)
    message(FATAL_ERROR "FS_ENABLE_THREAD_SANITIZER is not supported by MSVC")
  endif()
  message(STATUS "Sanitizers enabled: thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread)
endif()

if(MSVC)
  # The sources are ASCII and stay that way, which already settles the question: every byte means
  # the same in every codepage. This is the second line of defence, for the window between
  # someone typing an em dash and the linter telling them not to -- without it MSVC reads the
  # file in the system codepage and warns (C4819) or mangles the character.
  add_compile_options(/utf-8)
endif()

# Mirrors c_flags in build.zig. /W4 goes in without a /W3 to fight because cmake_policy(VERSION
# 3.18.2) at the top puts CMP0092 at NEW, which keeps CMake from seeding CMAKE_C_FLAGS with /W3;
# were it OLD, both would arrive and MSVC would answer D9025.
if(MSVC)
  set(FS_WARNING_FLAGS /W4)
else()
  set(FS_WARNING_FLAGS -Wall -Wextra)
endif()

# --- dependencies ---------------------------------------------------------------------------
#
# The commits are the ones build.zig.zon pins. Two lists of the same versions is the cost of
# having two builds; when they disagree, build.zig.zon is the one that is right.

# gradido-blockchain-core. Its own CMakeLists fetches libsodium (libsodium-cmake, which is what
# makes this work on MSVC at all) when USE_SODIUM is on, and arnm, whose sources it compiles
# into its own target -- so asking for it here is enough to get all three. The sodium target it
# brings is what service-core's jwt.c links against.
#
# The commit and not the tag: a tag can be moved, and this build has no business taking whatever
# it resolves to today. It is v0.17.1.
set(USE_SODIUM ON CACHE BOOL "" FORCE)
FetchContent_Declare(blockchain_core
  GIT_REPOSITORY https://github.com/gradido/gradido-blockchain-core.git
  GIT_TAG 9aaf20d2fb603a7bbd0c19ff3f113b29ea0de4f8
)
FetchContent_MakeAvailable(blockchain_core)

# arnm, for its headers only. The core populated it and compiled its sources -- yyjson included
# -- into gradido_blockchain_core, so there is nothing to fetch again and nothing to link; what
# is needed here is where it landed. FetchContent records that globally, which is what makes it
# readable from this scope although another CMakeLists did the populating.
#
# Do not pin yyjson here: two definitions of every yyjson_* symbol is a link-order lottery.
FetchContent_GetProperties(arnm)
if(NOT arnm_SOURCE_DIR)
  message(FATAL_ERROR
    "gradido-blockchain-core did not populate arnm. If a newer one stops, service-core needs "
    "another source for arnm/ and for yyjson.h.")
endif()
message(STATUS "arnm from ${arnm_SOURCE_DIR}")

# libuv is the platform layer -- the session cache's reader/writer lock, the log's mutex and the
# thread each role runs on all come from its loop-free half -- so every build gets it, not only
# the one that also uses it as the fallback backend's platform. AGENTS.md section 3a holds the
# decision. Upstream's own CMakeLists, which is what the zig build's package wraps as well.
set(LIBUV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(LIBUV_BUILD_BENCH OFF CACHE BOOL "" FORCE)
FetchContent_Declare(libuv
  GIT_REPOSITORY https://github.com/libuv/libuv.git
  GIT_TAG v1.52.0
)
FetchContent_MakeAvailable(libuv)

if(NOT FS_ENABLE_H2O)
  # picohttpparser, the parser half. It has no build system of its own and lives as a submodule
  # of h2o, so what is fetched is h2o -- shallow, and with that one submodule rather than the
  # dozen it otherwise carries.
  #
  # SOURCE_SUBDIR points at a directory with no CMakeLists.txt on purpose: it is what keeps
  # MakeAvailable from calling add_subdirectory() and pulling h2o's whole build into this one.
  # Only picohttpparser.c is compiled below, because upstream ships a bench.c and a test.c
  # beside it and each of those carries a main().
  FetchContent_Declare(h2o
    GIT_REPOSITORY https://github.com/h2o/h2o.git
    GIT_TAG 706842c0f8c0d9422efb97a4d8ef7d6ec9df87b7
    GIT_SUBMODULES deps/picohttpparser
    GIT_SUBMODULES_RECURSE OFF
    GIT_SHALLOW FALSE
    SOURCE_SUBDIR deps/picohttpparser
  )
  FetchContent_MakeAvailable(h2o)
  set(FS_PICOHTTPPARSER_DIR ${h2o_SOURCE_DIR}/deps/picohttpparser)
endif()

if(FS_ENABLE_SQLITE)
  # The amalgamation, from sqlite.org and not through anyone's build.zig or CMake package: it is
  # one C file. Same archive build.zig.zon pins, and the hash is checked here for the reason it
  # is pinned there -- a driver that is fetched over the network is part of the process that
  # signs transactions.
  #
  # SOURCE_SUBDIR points at a directory with no CMakeLists.txt for the same reason the h2o entry
  # above does: the archive has none, and MakeAvailable must not go looking for one.
  FetchContent_Declare(sqlite3
    URL https://sqlite.org/2026/sqlite-amalgamation-3530200.zip
    URL_HASH SHA256=8a310d0a16c7a90cacd4c884e70faa51c902afed2a89f63aaa0126ab83558a32
    SOURCE_SUBDIR does-not-exist
  )
  FetchContent_MakeAvailable(sqlite3)

  # The compile-time options are build.zig's list, with the reason for each beside it there.
  add_library(fs_sqlite3 STATIC ${sqlite3_SOURCE_DIR}/sqlite3.c)
  target_include_directories(fs_sqlite3 PUBLIC ${sqlite3_SOURCE_DIR})
  target_compile_definitions(fs_sqlite3 PRIVATE
    SQLITE_THREADSAFE=1
    SQLITE_DQS=0
    SQLITE_DEFAULT_WAL_SYNCHRONOUS=1
    SQLITE_DEFAULT_MEMSTATUS=0
    SQLITE_MAX_EXPR_DEPTH=0
    SQLITE_LIKE_DOESNT_MATCH_BLOBS=1
    SQLITE_OMIT_DEPRECATED=1
    SQLITE_OMIT_LOAD_EXTENSION=1
    SQLITE_OMIT_SHARED_CACHE=1
    SQLITE_USE_ALLOCA=1
  )
  # Upstream's, and not ours to keep clean under our warning flags.
  if(MSVC)
    target_compile_options(fs_sqlite3 PRIVATE /W0)
  else()
    target_compile_options(fs_sqlite3 PRIVATE -w)
  endif()
endif()

if(FS_ENABLE_TESTS)
  # googletest is C++ testing C, which is what arnm and gradido-blockchain-core do as well. The
  # language is enabled here rather than in project() so that a build without tests never asks
  # for a C++ compiler it does not need.
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 17)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)

  # Same version build.zig.zon pins, so the three repositories test against one googletest.
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.15.2
  )
  FetchContent_MakeAvailable(googletest)
  enable_testing()
endif()

# --- components -----------------------------------------------------------------------------

# The two HTTP backends live beside each other in service-core/src; exactly one is compiled.
file(GLOB SERVICE_CORE_SRC "service-core/src/*.c")
if(FS_ENABLE_H2O)
  list(REMOVE_ITEM SERVICE_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/service-core/src/http_fallback.c")
else()
  list(REMOVE_ITEM SERVICE_CORE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/service-core/src/http_h2o.c")
  list(APPEND SERVICE_CORE_SRC "${FS_PICOHTTPPARSER_DIR}/picohttpparser.c")
endif()

file(GLOB BACKEND_CORE_SRC "backend-core/src/*.c")
file(GLOB BACKEND_SRC "backend/src/*.c")
file(GLOB FEDERATION_SRC "federation/src/*.c")
# dht-node's Rust half does not exist yet; src/ holds the C stand-in behind the same extern "C"
# header. When the crate lands, this is where cargo gets invoked and the stub drops out.
file(GLOB DHT_NODE_SRC "dht-node/src/*.c")

# Every component's public headers on every component's search path -- same as build.zig, and for
# the same reason: what may depend on what is in Architecture.md, and a #include that breaks it is
# a review finding rather than a link error.
set(FS_INCLUDE_DIRS
  ${CMAKE_CURRENT_SOURCE_DIR}/service-core/include
  ${CMAKE_CURRENT_SOURCE_DIR}/backend-core/include
  ${CMAKE_CURRENT_SOURCE_DIR}/backend/include
  ${CMAKE_CURRENT_SOURCE_DIR}/federation/include
  ${CMAKE_CURRENT_SOURCE_DIR}/dht-node/include
)

function(fs_add_component name)
  add_library(${name} STATIC ${ARGN})
  target_include_directories(${name} PUBLIC ${FS_INCLUDE_DIRS})
  target_compile_options(${name} PRIVATE ${FS_WARNING_FLAGS})
  if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(${name} PUBLIC Threads::Threads)
  endif()
endfunction()

fs_add_component(service_core ${SERVICE_CORE_SRC})
fs_add_component(backend_core ${BACKEND_CORE_SRC})
fs_add_component(backend ${BACKEND_SRC})
fs_add_component(federation ${FEDERATION_SRC})
fs_add_component(dht_node ${DHT_NODE_SRC})

# The core hides its crypto declarations behind this macro, so a consumer that does not define it
# sees a different header than the one that was compiled.
foreach(target service_core backend_core)
  target_compile_definitions(${target} PRIVATE USE_SODIUM)
  # uv_a and not uv: the static one, matching what the zig build links.
  target_link_libraries(${target} PUBLIC gradido_blockchain_core sodium uv_a)
  target_include_directories(${target} PRIVATE
    ${blockchain_core_SOURCE_DIR}/include
    # data/unit.h reaches for "r128/r128.h", which the core vendors rather than installs.
    ${blockchain_core_SOURCE_DIR}/third_party
    # arnm_result is what a grd* call answers with, so a consumer of the core's public headers
    # needs arnm's include path as well.
    ${arnm_SOURCE_DIR}/include
  )
endforeach()

# libcurl. service-core's mail.c speaks SMTP through it, and the outbound HTTP this project
# will grow goes the same way.
#
# Taken from the host rather than built, which is this file's arrangement with h2o as well and
# for the same reason: `zig build` compiles the pinned allyourcodebase/curl and needs nothing
# installed, while this build exists for the one target zig cannot serve and asks the system for
# what is already there. So the version here is whatever the host has, and its TLS backend is
# whatever the distribution linked it against -- neither is the pinned combination build.zig
# produces. When the two disagree, build.zig.zon is right.
find_package(CURL REQUIRED)
target_link_libraries(service_core PUBLIC CURL::libcurl)

# The database drivers. db_postgres.c and db_sqlite.c are compiled either way and answer
# SC_ERR_UNAVAILABLE without their driver, so what these switch is what the binary can open --
# not whether it builds. service_core/db.h holds the arrangement.
if(FS_ENABLE_SQLITE)
  target_link_libraries(service_core PUBLIC fs_sqlite3)
  target_compile_definitions(service_core PRIVATE SC_DB_WITH_SQLITE=1)
endif()

if(FS_ENABLE_POSTGRES)
  find_package(PostgreSQL REQUIRED)
  target_link_libraries(service_core PUBLIC PostgreSQL::PostgreSQL)
  target_compile_definitions(service_core PRIVATE SC_DB_WITH_POSTGRESQL=1)
endif()

if(FS_ENABLE_H2O)
  # Unlike build.zig, this file does not build h2o -- zig fetches the pinned checkout and
  # compiles the source list itself, which is a page of build.zig and would be a second page
  # here. The CMake build asks the system for a libh2o-evloop that is already there.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(H2O libh2o-evloop)
  if(NOT H2O_FOUND)
    message(FATAL_ERROR
      "FS_ENABLE_H2O=ON but pkg-config cannot see libh2o-evloop.\n"
      "  This build does not compile h2o -- `zig build` does, and needs nothing installed.\n"
      "  Either install libh2o-evloop, or leave FS_ENABLE_H2O off and get the\n"
      "  libuv+picohttpparser backend, which needs nothing either.")
  endif()
  target_include_directories(service_core PRIVATE ${H2O_INCLUDE_DIRS})
  target_compile_definitions(service_core PRIVATE H2O_USE_LIBUV=0)
  target_link_directories(service_core PUBLIC ${H2O_LIBRARY_DIRS})
  target_link_libraries(service_core PUBLIC ${H2O_LIBRARIES})
else()
  target_include_directories(service_core PRIVATE ${FS_PICOHTTPPARSER_DIR})
  # picohttpparser is upstream's and is not ours to keep clean under our warning flags.
  if(MSVC)
    set_source_files_properties(${FS_PICOHTTPPARSER_DIR}/picohttpparser.c PROPERTIES COMPILE_OPTIONS "/W0")
  else()
    set_source_files_properties(${FS_PICOHTTPPARSER_DIR}/picohttpparser.c PROPERTIES COMPILE_OPTIONS "-w")
  endif()
endif()

target_link_libraries(backend PUBLIC backend_core service_core)
target_link_libraries(federation PUBLIC backend_core service_core)
target_link_libraries(dht_node PUBLIC service_core)
target_link_libraries(backend_core PUBLIC service_core)

add_executable(fast-servers src/main.c)
target_include_directories(fast-servers PRIVATE ${FS_INCLUDE_DIRS})
target_compile_options(fast-servers PRIVATE ${FS_WARNING_FLAGS})
# main.c runs each role on a uv_thread_t; the rest arrives through service_core.
target_link_libraries(fast-servers PRIVATE backend federation dht_node backend_core service_core uv_a)

if(MSVC)
  # debug symbols for a parent project stepping into this one
  target_compile_options(fast-servers PRIVATE $<$<CONFIG:Debug>:/JMC>)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT fast-servers)
endif()

if(FS_ENABLE_TESTS)
  # Unit tests live beside the component they test rather than in one tree at the root -- see
  # README.md, and build.zig, which explains why this project departs from its siblings here.
  add_subdirectory(service-core/tests)
  add_subdirectory(tests/integration)
endif()

if(FS_ENABLE_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()

install(TARGETS fast-servers RUNTIME DESTINATION bin)
