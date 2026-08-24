# The server the integration suite drives. Built here, run by `bun test` in this directory --
# see README.md. CMake does not run bun: a build step that shells out to another toolchain fails
# for reasons that have nothing to do with the build.

add_executable(http-probe probe/http_probe.c)
target_include_directories(http-probe PRIVATE ${FS_INCLUDE_DIRS})
target_compile_options(http-probe PRIVATE ${FS_WARNING_FLAGS})
# service_core carries the HTTP backend and the core forward as PUBLIC dependencies, so linking
# it is enough -- the probe is written against service_core/http.h and nothing else.
target_link_libraries(http-probe PRIVATE service_core)
