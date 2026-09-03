const std = @import("std");
const builtin = @import("builtin");
const zcc = @import("compile_commands");

/// Flags every first-party C source is compiled with.
///
/// -Wall and -Wextra are the baseline and the tree is clean under them, so anything they print
/// is new. CMakeLists.txt mirrors this, MSVC spelling included.
///
/// -Wconversion is deliberately absent, unlike in arnm. It earns its place there because
/// that library narrows on purpose and often; here the narrowing-heavy code is the money
/// arithmetic, which lives in gradido-blockchain-core and is compiled by its own build. What
/// this project would get instead is a page of findings from the h2o headers, which are not
/// ours to keep clean and which would bury the ones that are.
///
/// gnu11 and not c11: clock_gettime, CLOCK_MONOTONIC and the socket calls are POSIX rather than
/// ISO C, and strict -std=c11 leaves the feature test macros that reveal them undefined.
///
/// Where these become visible is not obvious: `zig build` drops C compiler warnings when a step
/// succeeds and relabels them as errors when one fails, so a green build says nothing either
/// way. They surface in the editor through compile_commands.json, and in the CMake build, which
/// prints them as the warnings they are.
const c_flags = [_][]const u8{ "-std=gnu11", "-Wall", "-Wextra" };

/// Recursively adds the .c files under @p dir_path.
///
/// @p skip names what the walk leaves alone, relative to @p dir_path -- here, the HTTP backend
/// this build did not select. Write its entries with '/' whatever the host is; the walk
/// converts them to the host separator before comparing.
fn addDirSources(
    compile: *std.Build.Step.Compile,
    b: *std.Build,
    dir_path: []const u8,
    skip: []const []const u8,
) void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.panic("Failed to open directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer dir.close();

    var walker = dir.walk(b.allocator) catch |err| {
        std.debug.panic("Failed to walk directory '{s}': {s}", .{ dir_path, @errorName(err) });
    };
    defer walker.deinit();

    const skip_native = b.allocator.alloc([]const u8, skip.len) catch @panic("OOM");
    for (skip, skip_native) |written, *native| {
        const copy = b.dupe(written);
        std.mem.replaceScalar(u8, copy, '/', std.fs.path.sep);
        native.* = copy;
    }

    outer: while (walker.next() catch null) |entry| {
        if (entry.kind != .file or !std.mem.endsWith(u8, entry.path, ".c")) continue;
        for (skip_native) |skipped| {
            if (std.mem.eql(u8, entry.path, skipped)) continue :outer;
            const prefix = b.fmt("{s}{c}", .{ skipped, std.fs.path.sep });
            if (std.mem.startsWith(u8, entry.path, prefix)) continue :outer;
        }
        compile.addCSourceFiles(.{
            .files = &.{b.fmt("{s}/{s}", .{ dir_path, entry.path })},
            .flags = &c_flags,
        });
    }
}

/// Sanitizers the zig toolchain can apply to the C sources of this project.
///
/// AddressSanitizer is deliberately absent: zig does not ship the asan runtime, so
/// `-fsanitize=address` would compile but fail to link. Use the CMake build with
/// `-DFS_ENABLE_SANITIZERS=ON` for a leak and out-of-bounds check.
///
/// AGENTS.md section 4 asks for all three in CI, and names ThreadSanitizer as the one that
/// matters most here -- the session cache's reference counting and its two lock layers are the
/// defect class expert review does not catch.
const SanitizeMode = enum {
    /// no instrumentation
    off,
    /// UndefinedBehaviorSanitizer, aborting with a diagnostic on the first finding
    undefined_behavior,
    /// ThreadSanitizer, reporting data races between threads
    thread,
};

/// Instruments a module according to @p mode. Applied to every target of the build, so library
/// and binary always agree -- mixing an instrumented library with an uninstrumented binary
/// produces false reports.
fn applySanitize(module: *std.Build.Module, mode: SanitizeMode) void {
    switch (mode) {
        .off => {},
        .undefined_behavior => module.sanitize_c = .full,
        .thread => module.sanitize_thread = true,
    }
}

/// True when @p target names the machine this build runs on.
///
/// Either by being native, or by spelling the host's own triple out --
/// `-Dtarget=x86_64-linux-gnu` on an x86_64 Debian is the same machine, but zig treats it as a
/// cross build and stops consulting the system's headers. That distinction is invisible in the
/// error it produces, which is 78 lines of `'openssl/ssl.h' file not found`.
fn targetIsHost(target: std.Build.ResolvedTarget) bool {
    if (target.query.isNative()) return true;
    const t = target.result;
    return t.cpu.arch == builtin.target.cpu.arch and t.os.tag == builtin.target.os.tag and
        t.abi == builtin.target.abi;
}

/// Puts the host's own header and library directories on @p compile, for what zig does not carry.
///
/// Three of them, for two different reasons:
///
///   /usr/include                     OpenSSL and zlib, which h2o includes and which no
///                                    dependency of this build provides.
///   /usr/include/<arch>-linux-<abi>  On Debian and Ubuntu the more interesting half of libc,
///                                    `asm/errno.h` and friends, sits one level deeper under
///                                    the multiarch triple and zig's detection stops above it.
///   /usr/lib/<arch>-linux-<abi>      Where the -l flags for ssl, crypto and z resolve.
///
/// A native target already has the first and the third; a *named* host target has neither,
/// which is the whole reason this function exists. A target that is not the host gets nothing:
/// its headers and libraries are not these, and pretending otherwise builds a binary against
/// one machine's libraries for another's.
fn addHostSystemPaths(
    b: *std.Build,
    compile: *std.Build.Step.Compile,
    target: std.Build.ResolvedTarget,
) void {
    const t = target.result;
    if (!targetIsHost(target) or t.os.tag != .linux) return;
    if (!std.mem.startsWith(u8, @tagName(t.abi), "gnu") and
        !std.mem.startsWith(u8, @tagName(t.abi), "musl")) return;

    const triple = b.fmt("{s}-linux-{s}", .{ @tagName(t.cpu.arch), @tagName(t.abi) });
    const named_host = !target.query.isNative();

    if (named_host) compile.addSystemIncludePath(.{ .cwd_relative = "/usr/include" });

    const multiarch_include = b.fmt("/usr/include/{s}", .{triple});
    if (std.fs.accessAbsolute(multiarch_include, .{})) |_| {
        compile.addSystemIncludePath(.{ .cwd_relative = multiarch_include });
    } else |_| {}

    if (!named_host) return;
    for ([_][]const u8{ b.fmt("/usr/lib/{s}", .{triple}), "/usr/lib", "/lib" }) |dir| {
        std.fs.accessAbsolute(dir, .{}) catch continue;
        compile.addLibraryPath(.{ .cwd_relative = dir });
    }
}

/// A libc description Debian's native detection does not produce, for the compiles this build
/// does not own.
///
/// `addMultiarchIncludeDir` above fixes the targets declared in this file. It cannot fix the
/// ones zig compiles on its own account -- libtsan, built before it can be linked -- or the
/// ones a dependency declares, and libsodium is the second kind: its sources include `errno.h`,
/// which on Debian and Ubuntu reaches `asm/errno.h` under the multiarch triple, and
/// `zig libc` reports `sys_include_dir=/usr/include` regardless.
///
/// So the detected description is taken, its `sys_include_dir` is pointed at the multiarch
/// directory, and the result is handed to every target of this build -- from where it reaches
/// the dependencies as well. `--libc <file>` on the command line wins over it, and on a host
/// without that directory nothing is generated and nothing changes.
///
/// This is what makes `-Dsanitize=thread` run at all, and AGENTS.md section 4 asks for TSan in
/// CI: a sanitizer that needs a hand-written file to start is a sanitizer that does not run.
fn nativeLibcFile(b: *std.Build, target: std.Build.ResolvedTarget) ?std.Build.LazyPath {
    const t = target.result;
    if (b.libc_file != null) return null;
    if (!targetIsHost(target) or t.os.tag != .linux) return null;
    if (!std.mem.startsWith(u8, @tagName(t.abi), "gnu") and
        !std.mem.startsWith(u8, @tagName(t.abi), "musl")) return null;

    const multiarch = b.fmt("/usr/include/{s}-linux-{s}", .{ @tagName(t.cpu.arch), @tagName(t.abi) });
    std.fs.accessAbsolute(multiarch, .{}) catch return null;

    const detected = std.process.Child.run(.{
        .allocator = b.allocator,
        .argv = &.{ b.graph.zig_exe, "libc" },
    }) catch return null;
    if (detected.term != .Exited or detected.term.Exited != 0) return null;

    var patched: std.ArrayList(u8) = .empty;
    var lines = std.mem.splitScalar(u8, detected.stdout, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "sys_include_dir=")) {
            patched.appendSlice(b.allocator, b.fmt("sys_include_dir={s}\n", .{multiarch})) catch @panic("OOM");
        } else {
            patched.appendSlice(b.allocator, line) catch @panic("OOM");
            patched.append(b.allocator, '\n') catch @panic("OOM");
        }
    }
    return b.addWriteFiles().add("libc.txt", patched.items);
}

/// The source list of libh2o-evloop, copied from LIB_SOURCE_FILES in h2o's CMakeLists.txt.
/// QUIC and HTTP/3 come along for the ride: h2o's core refers to them whether or not anyone
/// intends to speak them.
const h2o_sources = [_][]const u8{
    "deps/cloexec/cloexec.c",
    "deps/hiredis/async.c",
    "deps/hiredis/hiredis.c",
    "deps/hiredis/net.c",
    "deps/hiredis/read.c",
    "deps/hiredis/sds.c",
    "deps/hiredis/alloc.c",
    "deps/libgkc/gkc.c",
    "deps/libyrmcds/close.c",
    "deps/libyrmcds/connect.c",
    "deps/libyrmcds/recv.c",
    "deps/libyrmcds/send.c",
    "deps/libyrmcds/send_text.c",
    "deps/libyrmcds/socket.c",
    "deps/libyrmcds/strerror.c",
    "deps/libyrmcds/text_mode.c",
    "deps/picohttpparser/picohttpparser.c",
    "deps/picotls/deps/cifra/src/blockwise.c",
    "deps/picotls/deps/cifra/src/chash.c",
    "deps/picotls/deps/cifra/src/curve25519.c",
    "deps/picotls/deps/cifra/src/drbg.c",
    "deps/picotls/deps/cifra/src/hmac.c",
    "deps/picotls/deps/cifra/src/sha256.c",
    "deps/picotls/lib/hpke.c",
    "deps/picotls/lib/pembase64.c",
    "deps/picotls/lib/picotls.c",
    "deps/picotls/lib/openssl.c",
    "deps/picotls/lib/cifra/random.c",
    "deps/picotls/lib/cifra/x25519.c",
    "deps/quicly/lib/cc-cubic.c",
    "deps/quicly/lib/cc-pico.c",
    "deps/quicly/lib/cc-reno.c",
    "deps/quicly/lib/defaults.c",
    "deps/quicly/lib/frame.c",
    "deps/quicly/lib/local_cid.c",
    "deps/quicly/lib/loss.c",
    "deps/quicly/lib/quicly.c",
    "deps/quicly/lib/ranges.c",
    "deps/quicly/lib/rate.c",
    "deps/quicly/lib/recvstate.c",
    "deps/quicly/lib/remote_cid.c",
    "deps/quicly/lib/sendstate.c",
    "deps/quicly/lib/sentmap.c",
    "deps/quicly/lib/streambuf.c",

    "lib/common/cache.c",
    "lib/common/file.c",
    "lib/common/filecache.c",
    "lib/common/hostinfo.c",
    "lib/common/http1client.c",
    "lib/common/http2client.c",
    "lib/common/http3client.c",
    "lib/common/httpclient.c",
    "lib/common/memcached.c",
    "lib/common/memory.c",
    "lib/common/multithread.c",
    "lib/common/redis.c",
    "lib/common/serverutil.c",
    "lib/common/socket.c",
    "lib/common/socketpool.c",
    "lib/common/string.c",
    "lib/common/rand.c",
    "lib/common/time.c",
    "lib/common/timerwheel.c",
    "lib/common/token.c",
    "lib/common/url.c",
    "lib/common/balancer/roundrobin.c",
    "lib/common/balancer/least_conn.c",
    "lib/common/absprio.c",

    "lib/core/config.c",
    "lib/core/configurator.c",
    "lib/core/context.c",
    "lib/core/headers.c",
    "lib/core/logconf.c",
    "lib/core/pipe_sender.c",
    "lib/core/proxy.c",
    "lib/core/request.c",
    "lib/core/util.c",

    "lib/handler/access_log.c",
    "lib/handler/compress.c",
    "lib/handler/compress/gzip.c",
    "lib/handler/errordoc.c",
    "lib/handler/expires.c",
    "lib/handler/fastcgi.c",
    "lib/handler/file.c",
    "lib/handler/h2olog.c",
    "lib/handler/headers.c",
    "lib/handler/headers_util.c",
    "lib/handler/http2_debug_state.c",
    "lib/handler/mimemap.c",
    "lib/handler/proxy.c",
    "lib/handler/connect.c",
    "lib/handler/redirect.c",
    "lib/handler/reproxy.c",
    "lib/handler/throttle_resp.c",
    "lib/handler/self_trace.c",
    "lib/handler/server_timing.c",
    "lib/handler/status.c",
    "lib/handler/status/events.c",
    "lib/handler/status/memory.c",
    "lib/handler/status/requests.c",
    "lib/handler/status/ssl.c",
    "lib/handler/status/durations.c",
    "lib/handler/configurator/access_log.c",
    "lib/handler/configurator/compress.c",
    "lib/handler/configurator/errordoc.c",
    "lib/handler/configurator/expires.c",
    "lib/handler/configurator/fastcgi.c",
    "lib/handler/configurator/file.c",
    "lib/handler/configurator/h2olog.c",
    "lib/handler/configurator/headers.c",
    "lib/handler/configurator/headers_util.c",
    "lib/handler/configurator/http2_debug_state.c",
    "lib/handler/configurator/proxy.c",
    "lib/handler/configurator/redirect.c",
    "lib/handler/configurator/reproxy.c",
    "lib/handler/configurator/throttle_resp.c",
    "lib/handler/configurator/self_trace.c",
    "lib/handler/configurator/server_timing.c",
    "lib/handler/configurator/status.c",

    "lib/http1.c",

    "lib/http2/cache_digests.c",
    "lib/http2/casper.c",
    "lib/http2/connection.c",
    "lib/http2/frame.c",
    "lib/http2/hpack.c",
    "lib/http2/scheduler.c",
    "lib/http2/stream.c",
    "lib/http2/http2_debug_state.c",

    "lib/http3/frame.c",
    "lib/http3/qpack.c",
    "lib/http3/common.c",
    "lib/http3/server.c",
};

/// libyaml. h2o's configurator reaches for yoml, yoml reaches for yaml, and the linker
/// eventually asks who is going to pay for all this.
const yaml_sources = [_][]const u8{
    "deps/yaml/src/api.c",
    "deps/yaml/src/dumper.c",
    "deps/yaml/src/emitter.c",
    "deps/yaml/src/loader.c",
    "deps/yaml/src/parser.c",
    "deps/yaml/src/reader.c",
    "deps/yaml/src/scanner.c",
    "deps/yaml/src/writer.c",
};

const h2o_include_dirs = [_][]const u8{
    "include",
    "deps/cloexec",
    "deps/golombset",
    "deps/hiredis",
    "deps/libgkc",
    "deps/libyrmcds",
    "deps/klib",
    "deps/neverbleed",
    "deps/picohttpparser",
    "deps/picotest",
    "deps/picotls/deps/cifra/src/ext",
    "deps/picotls/deps/cifra/src",
    "deps/picotls/deps/micro-ecc",
    "deps/picotls/include",
    "deps/quicly/include",
    "deps/yaml/include",
    "deps/yoml",
};

/// h2o's own sources, which are not ours to keep clean under -Wall -Wextra.
const h2o_c_flags = [_][]const u8{
    "-std=gnu99",
    "-Wno-unused-value",
    "-Wno-unused-function",
    "-Wno-unused-variable",
    "-Wno-unused-but-set-variable",
    "-Wno-implicit-fallthrough",
    "-Wno-sign-compare",
    "-Wno-missing-field-initializers",
    "-Wno-deprecated-declarations",
};

/// Builds libh2o-evloop from the upstream h2o checkout, which arrives without a build.zig and
/// without any sign of remorse about it. Lifted from the h2o prototype, where it was written.
///
/// @p tls and @p zlib are the two libraries h2o cannot be built without, and both used to
/// come from the host. They are parameters rather than fetched here so that the *same* two
/// artifacts reach the executable -- see the libressl entry in build.zig.zon for
/// what a second instance of OpenSSL in one process would mean.
///
/// Linking them here is also what puts their headers on h2o's search path: both install their
/// include directories, and `linkLibrary` carries those to whatever links the result. That is
/// why `#include <openssl/ssl.h>` in h2o/socket.h resolves without a single addIncludePath.
fn buildH2o(
    b: *std.Build,
    dep: *std.Build.Dependency,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    tls: *std.Build.Step.Compile,
    zlib: *std.Build.Step.Compile,
) *std.Build.Step.Compile {
    const lib = b.addLibrary(.{
        .name = "h2o-evloop",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });

    addHostSystemPaths(b, lib, target);
    lib.linkLibrary(tls);
    lib.linkLibrary(zlib);
    for (h2o_include_dirs) |dir| lib.addIncludePath(dep.path(dir));

    // The evloop backend, so libuv remains a stranger
    lib.root_module.addCMacro("H2O_USE_LIBUV", "0");
    lib.root_module.addCMacro("_GNU_SOURCE", "1");
    lib.root_module.addCMacro("H2O_ROOT", "\"/usr/local\"");
    lib.root_module.addCMacro("H2O_CONFIG_PATH", "\"/usr/local/etc/h2o.conf\"");

    lib.addCSourceFiles(.{ .root = dep.path("."), .files = &h2o_sources, .flags = &h2o_c_flags });
    lib.addCSourceFiles(.{ .root = dep.path("."), .files = &yaml_sources, .flags = &h2o_c_flags });

    return lib;
}

/// SQLite, compiled here out of sqlite.org's amalgamation -- see build.zig.zon, .sqlite3, for
/// why the package that wraps the same archive is not used.
///
/// The defines are upstream's own recommended compile-time options, minus the ones that would
/// remove something this project may want. Each is here for a stated reason and none is a
/// performance guess:
///
///   THREADSAFE=1            serialized. The roles are threads and a connection may be reached
///                           from more than one of them; SQLITE_THREADSAFE=2 would make that
///                           the caller's problem for no measurable gain at this size.
///   DQS=0                   "x" is a string literal and never an identifier fallback. That
///                           fallback turns a misspelled column into a silently constant
///                           string, which is the SQL version of the wrong-column-index bug
///                           Architecture.md wants generated code to remove.
///   OMIT_LOAD_EXTENSION     no dlopen reachable from a statement, in a process that signs
///                           transactions.
///   OMIT_SHARED_CACHE       upstream discourages it and its locking model is a source of
///                           surprises; WAL is what this build uses instead.
///   DEFAULT_WAL_SYNCHRONOUS=1  NORMAL under WAL: durable across a process crash, and a commit
///                           does not wait for the platter. FULL is for losing power mid-write.
///   DEFAULT_MEMSTATUS=0     drops the global allocation counters nothing here reads.
///   LIKE_DOESNT_MATCH_BLOBS, MAX_EXPR_DEPTH=0, OMIT_DEPRECATED, USE_ALLOCA
///                           the rest of upstream's list, kept together so it stays checkable
///                           against https://sqlite.org/compile.html#recommended_compile_time_options
///
/// The amalgamation is not compiled under this project's warning flags: it is not ours to keep
/// clean under them, and the same rule already applies to picohttpparser.
fn buildSqlite(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    sanitize: SanitizeMode,
) *std.Build.Step.Compile {
    const dep = b.dependency("sqlite3", .{});
    const lib = b.addLibrary(.{
        .name = "sqlite3",
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    // The database is reached from the role threads, so it is instrumented with everything
    // else -- an uninstrumented library under a TSan build reports nothing and hides what the
    // rest of the build is looking for.
    applySanitize(lib.root_module, sanitize);
    addHostSystemPaths(b, lib, target);

    const defines = [_][2][]const u8{
        .{ "SQLITE_THREADSAFE", "1" },
        .{ "SQLITE_DQS", "0" },
        .{ "SQLITE_DEFAULT_WAL_SYNCHRONOUS", "1" },
        .{ "SQLITE_DEFAULT_MEMSTATUS", "0" },
        .{ "SQLITE_MAX_EXPR_DEPTH", "0" },
        .{ "SQLITE_LIKE_DOESNT_MATCH_BLOBS", "1" },
        .{ "SQLITE_OMIT_DEPRECATED", "1" },
        .{ "SQLITE_OMIT_LOAD_EXTENSION", "1" },
        .{ "SQLITE_OMIT_SHARED_CACHE", "1" },
        .{ "SQLITE_USE_ALLOCA", "1" },
    };
    for (defines) |define| lib.root_module.addCMacro(define[0], define[1]);

    lib.addCSourceFiles(.{ .root = dep.path("."), .files = &.{"sqlite3.c"}, .flags = &.{"-std=gnu11"} });
    // db_sqlite.c includes <sqlite3.h>; installing it here is what puts it on the path of
    // whatever links this library, the way the libressl and curl packages do it.
    lib.installHeader(dep.path("sqlite3.h"), "sqlite3.h");
    return lib;
}

/// Every component's public headers, on every component's search path.
///
/// The alternative -- naming per target which of the five a translation unit is allowed to see
/// -- would be a layering rule enforced by the build, and the build is the wrong place for it:
/// what may depend on what is in Architecture.md, and a `#include` that breaks it is a review
/// finding, not a link error. Six directories is also simply cheaper to keep correct than six
/// lists of directories.
const component_includes = [_][]const u8{
    "service-core/include",
    "backend-core/include",
    "backend/include",
    "federation/include",
    "dht-node/include",
};

fn addComponentIncludes(b: *std.Build, compile: *std.Build.Step.Compile) void {
    for (component_includes) |dir| compile.addIncludePath(b.path(dir));
}

/// Links whichever HTTP backend the build selected, plus the system libraries it needs.
///
/// Two binaries want this: the server and the integration probe. @p backend_lib is optional
/// because a lazy dependency is null on the pass that fetches it, and on that pass nothing is
/// built anyway -- the build re-runs afterwards.
fn linkHttpBackend(
    compile: *std.Build.Step.Compile,
    backend_lib: ?*std.Build.Step.Compile,
    system_libs: []const []const u8,
) void {
    if (backend_lib) |lib| compile.linkLibrary(lib);
    for (system_libs) |name| compile.linkSystemLibrary(name);
}

/// One googletest binary: which component it links and where its source is.
const UnitTest = struct {
    name: []const u8,
    /// Relative to the build root, e.g. "service-core/tests".
    dir: []const u8,
    src: []const u8,
    /// The component under test, or null for one whose sources are compiled straight in.
    lib: ?*std.Build.Step.Compile = null,
    /// Sources compiled into the test rather than linked.
    ///
    /// For a leaf that depends on nothing: `backend/src/field_rules.c` is the rules of the
    /// shared schemas and reaches no header outside its own directory, so compiling it here
    /// tests it without dragging the HTTP backend into a unit test through the role that uses
    /// it. Anything with a dependency is linked as a library instead, which is what keeps the
    /// include rule below meaningful.
    sources: []const []const u8 = &.{},
    /// Libraries the component under test is itself built against.
    deps: []const *std.Build.Step.Compile = &.{},
    /// Directories beyond the component's own `include/`.
    ///
    /// The rule is that a test sees what its component may see and nothing else, which for
    /// service-core is its own include directory alone. backend-core is written against
    /// service-core's headers and links it, so its test needs that one too -- and a private
    /// header a component keeps in `src/` is reached by naming that directory here. What the
    /// rule forbids is the other four components' headers, which is what a shared test tree
    /// would hand out for free.
    includes: []const []const u8 = &.{},
};

const Context = struct {
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    sanitize: SanitizeMode,
    /// Non-null on a Debian-style multiarch host; see nativeLibcFile.
    libc_file: ?std.Build.LazyPath,
    cdb: *std.ArrayList(*std.Build.Step.Compile),
};

/// contracts/migrations/, turned into a C source the binary carries.
///
/// The migrations are a contract both implementations run, and neither reads them off disk:
/// each has to stay shippable as a single binary, so the TypeScript path imports them as text
/// and this one has its build write them out. That is build work and not application code --
/// the set of migrations is still decided at runtime, by contracts/migrations/index.json, which
/// is embedded along with the SQL. See backend-core/include/backend_core/database/contract_files.h.
///
/// Every file under the directory is taken, sorted, and emitted as bytes. Sorted because the
/// output has to be byte-identical for the same input, and a directory walk is not ordered;
/// bytes rather than string literals because MSVC has a limit on those and the CMake build
/// emits the same shape from the same files.
fn migrationsSource(b: *std.Build) std.Build.LazyPath {
    const root = "../contracts/migrations";
    var out: std.ArrayList(u8) = .empty;
    const w = out.writer(b.allocator);

    w.print("/* Generated by build.zig from {s}. Do not edit, do not check in. */\n", .{root}) catch @panic("OOM");
    w.writeAll("#include \"backend_core/database/contract_files.h\"\n\n") catch @panic("OOM");

    var dir = b.build_root.handle.openDir(root, .{ .iterate = true }) catch |err| {
        std.debug.panic("cannot open '{s}': {s}", .{ root, @errorName(err) });
    };
    defer dir.close();

    var paths: std.ArrayList([]const u8) = .empty;
    var walker = dir.walk(b.allocator) catch @panic("OOM");
    defer walker.deinit();
    while (walker.next() catch null) |entry| {
        if (entry.kind != .file) continue;
        const copy = b.dupe(entry.path);
        std.mem.replaceScalar(u8, copy, std.fs.path.sep, '/');
        paths.append(b.allocator, copy) catch @panic("OOM");
    }
    std.mem.sort([]const u8, paths.items, {}, struct {
        fn lessThan(_: void, a: []const u8, c: []const u8) bool {
            return std.mem.order(u8, a, c) == .lt;
        }
    }.lessThan);

    for (paths.items, 0..) |path, index| {
        const bytes = dir.readFileAlloc(b.allocator, path, 4 * 1024 * 1024) catch |err| {
            std.debug.panic("cannot read '{s}/{s}': {s}", .{ root, path, @errorName(err) });
        };
        w.print("static const char kFile{d}[] = {{", .{index}) catch @panic("OOM");
        for (bytes, 0..) |byte, i| {
            if (i % 16 == 0) w.writeAll("\n    ") catch @panic("OOM");
            w.print("{d},", .{byte}) catch @panic("OOM");
        }
        // The terminator the header promises, beyond the length the table reports.
        w.writeAll("\n    0};\n") catch @panic("OOM");
    }

    w.writeAll("\nconst bc_contract_file bc_migration_files[] = {\n") catch @panic("OOM");
    for (paths.items, 0..) |path, index| {
        const bytes = dir.readFileAlloc(b.allocator, path, 4 * 1024 * 1024) catch @panic("OOM");
        w.print("    {{\"{s}\", kFile{d}, {d}}},\n", .{ path, index, bytes.len }) catch @panic("OOM");
    }
    w.writeAll("};\n\n") catch @panic("OOM");
    w.print("const size_t bc_migration_file_count = {d};\n", .{paths.items.len}) catch @panic("OOM");

    return b.addWriteFiles().add("migrations_data.gen.c", out.items);
}

/// One site of `publish/sites.json`, and one file of one site.
///
/// Only what this build reads. `ignore_unknown_fields` is on where it is parsed, so a field
/// added for the TypeScript bundler does not break the C build.
const PublishedFile = struct {
    path: []const u8,
    /// Spelled with @"" because `type` names a primitive here and a JSON key there.
    @"type": []const u8,
    etag: []const u8,
    immutable: bool,
};

const PublishedSite = struct {
    name: []const u8,
    /// camelCase because that is the key in the JSON, and std.json matches field names.
    basePath: []const u8,
    dir: []const u8,
    index: []const u8,
    files: []const PublishedFile,
};

const Manifest = struct {
    sites: []const PublishedSite,
};

/// `publish/`, turned into a C source the binary carries.
///
/// The same arrangement as migrationsSource above and for the same reason -- a server has to
/// stay shippable as one file, and a page read off the disk of whoever runs the binary is not
/// in one. What differs is where the bytes come from: `publish/` is built by the TypeScript
/// toolchain, because a page is vite's output and this path has no JavaScript in it.
///
/// **What is embedded is what `publish/sites.json` names**, not what the directory happens to
/// hold, and the content type and the ETag are read from there rather than worked out here.
/// The TypeScript bundler reads the same file, so the two implementations serve the same bytes
/// under the same headers. See backend/include/backend/static_sites.h.
///
/// With no manifest -- `-Dpages=false`, or a machine with no bun -- this writes the same table
/// with no sites in it, and the server answers every path the way it did before there were any.
fn staticSitesSource(b: *std.Build, publish_root: []const u8, enabled: bool) std.Build.LazyPath {
    var out: std.ArrayList(u8) = .empty;
    const w = out.writer(b.allocator);

    w.print("/* Generated by build.zig from {s}. Do not edit, do not check in. */\n", .{publish_root}) catch @panic("OOM");
    w.writeAll("#include \"backend/static_sites.h\"\n\n") catch @panic("OOM");

    const manifest: ?Manifest = if (enabled) readManifest(b, publish_root) else null;

    if (manifest) |sites_json| {
        for (sites_json.sites, 0..) |site, site_index| {
            var dir = b.build_root.handle.openDir(b.pathJoin(&.{ publish_root, site.dir }), .{}) catch |err| {
                std.debug.panic("cannot open '{s}/{s}': {s} -- run `bun run publish`", .{ publish_root, site.dir, @errorName(err) });
            };
            defer dir.close();

            for (site.files, 0..) |file, file_index| {
                // 64 MB is not a size limit anybody should reach; it is the point at which a
                // file in publish/ is a mistake rather than a page, and reading it into a build
                // would be the first thing to notice.
                const bytes = dir.readFileAlloc(b.allocator, file.path, 64 * 1024 * 1024) catch |err| {
                    std.debug.panic("cannot read '{s}/{s}/{s}': {s}", .{ publish_root, site.dir, file.path, @errorName(err) });
                };
                w.print("static const char kS{d}F{d}[] = {{", .{ site_index, file_index }) catch @panic("OOM");
                for (bytes, 0..) |byte, i| {
                    if (i % 16 == 0) w.writeAll("\n    ") catch @panic("OOM");
                    w.print("{d},", .{byte}) catch @panic("OOM");
                }
                // Terminated beyond its length, so a caller that wants a C string has one.
                w.print("\n    0}}; /* {s}, {d} bytes */\n", .{ file.path, bytes.len }) catch @panic("OOM");
            }

            w.print("\nstatic const backend_static_file kS{d}Files[] = {{\n", .{site_index}) catch @panic("OOM");
            for (site.files, 0..) |file, file_index| {
                const bytes = dir.readFileAlloc(b.allocator, file.path, 64 * 1024 * 1024) catch @panic("OOM");
                w.print(
                    "    {{\"{s}\", {d}, kS{d}F{d}, {d}, \"{s}\", \"\\\"{s}\\\"\", {d}}},\n",
                    .{ file.path, file.path.len, site_index, file_index, bytes.len, file.@"type", file.etag, @intFromBool(file.immutable) },
                ) catch @panic("OOM");
            }
            w.writeAll("};\n") catch @panic("OOM");
        }
    }

    w.writeAll("\nconst backend_static_site backend_static_sites[] = {\n") catch @panic("OOM");
    if (manifest) |sites_json| {
        for (sites_json.sites, 0..) |site, site_index| {
            const index_at = indexOfFile(site) orelse std.debug.panic(
                "publish/sites.json: site '{s}' names an index '{s}' that is not one of its files",
                .{ site.name, site.index },
            );
            w.print("    {{\"{s}\", \"{s}\", {d}, kS{d}Files, {d}, &kS{d}Files[{d}]}},\n", .{
                site.name, site.basePath, site.basePath.len, site_index, site.files.len, site_index, index_at,
            }) catch @panic("OOM");
        }
    } else {
        // A zero-length array is not C, and an empty one still has to be a definition the
        // linker can resolve -- the count below is what makes it never read.
        w.writeAll("    {0}\n") catch @panic("OOM");
    }
    w.writeAll("};\n\n") catch @panic("OOM");
    w.print("const size_t backend_static_site_count = {d};\n", .{if (manifest) |m| m.sites.len else 0}) catch @panic("OOM");

    return b.addWriteFiles().add("static_sites_data.gen.c", out.items);
}

fn indexOfFile(site: PublishedSite) ?usize {
    for (site.files, 0..) |file, i| {
        if (std.mem.eql(u8, file.path, site.index)) return i;
    }
    return null;
}

/// Reads `publish/sites.json`, or explains what to run.
fn readManifest(b: *std.Build, publish_root: []const u8) Manifest {
    const path = b.pathJoin(&.{ publish_root, "sites.json" });
    const bytes = b.build_root.handle.readFileAlloc(b.allocator, path, 8 * 1024 * 1024) catch |err| {
        std.debug.panic(
            "cannot read '{s}': {s}\n" ++
                "  The pages come from the TypeScript build: run `turbo publish` in the\n" ++
                "  repository root, or build without them with -Dpages=false.",
            .{ path, @errorName(err) },
        );
    };
    // Unknown fields are ignored on purpose: the manifest is also read by scripts/bundle.ts,
    // and a field added for that reader is not a reason for this one to stop.
    const parsed = std.json.parseFromSlice(Manifest, b.allocator, bytes, .{ .ignore_unknown_fields = true }) catch |err| {
        std.debug.panic("'{s}' is not the manifest this build understands: {s}", .{ path, @errorName(err) });
    };
    return parsed.value;
}

/// Builds the pages, by running the TypeScript toolchain that owns them.
///
/// This is the one place the C build reaches for bun, and it is worth being plain about the
/// cost: `zig build` alone no longer produces a *complete* server on a machine without it.
/// The alternative was checking a megabyte of generated C into the repository on every frontend
/// change, and the honest split is the one below -- the *server* still builds with zig alone,
/// and `-Dpages=false` says so; what needs bun is the half of it that bun produced.
///
/// **Through turbo, and that is the whole of "do not do it twice".** `publish` is a task with
/// declared inputs and outputs, so a call that changes nothing is a cache hit in a few
/// milliseconds -- which is what a `bun bundle` building both implementations makes of the
/// second call. Whether the work is needed is turbo's question to answer, and a build that
/// answered it itself with a flag would be a second, worse cache.
///
/// A failure is not fatal when there is already a `publish/` to fall back on: a rebuild with no
/// network, or with bun missing, then serves the pages of the last publish rather than refusing
/// to build at all.
fn runPublish(b: *std.Build, publish_root: []const u8) void {
    const repo = b.pathFromRoot("..");
    // `bun x` rather than a bare `turbo`: the repository pins its own, and a build that silently
    // used whichever turbo is on the PATH is a build nobody can reproduce.
    var child = std.process.Child.init(&.{ "bun", "x", "turbo", "publish" }, b.allocator);
    child.cwd = repo;
    child.stdin_behavior = .Ignore;
    child.stdout_behavior = .Inherit;
    child.stderr_behavior = .Inherit;

    const term = child.spawnAndWait() catch |err| {
        onPublishFailure(b, publish_root, @errorName(err));
        return;
    };
    switch (term) {
        .Exited => |code| if (code != 0) onPublishFailure(b, publish_root, b.fmt("exit code {d}", .{code})),
        else => onPublishFailure(b, publish_root, "it was killed"),
    }
}

fn onPublishFailure(b: *std.Build, publish_root: []const u8, why: []const u8) void {
    const manifest = b.pathJoin(&.{ publish_root, "sites.json" });
    if (b.build_root.handle.access(manifest, .{})) |_| {
        std.log.warn("`turbo publish` did not run ({s}); using the pages already in {s}", .{ why, publish_root });
    } else |_| {
        std.debug.panic(
            "`turbo publish` failed ({s}) and there is no {s} to fall back on.\n" ++
                "  The pages are built by the TypeScript toolchain -- install bun, or build\n" ++
                "  the server without them with -Dpages=false.",
            .{ why, manifest },
        );
    }
}

/// One static library per component, all built the same way.
fn addComponent(
    context: *const Context,
    name: []const u8,
    src_dir: []const u8,
    skip: []const []const u8,
) *std.Build.Step.Compile {
    const b = context.b;
    const lib = b.addLibrary(.{
        .name = name,
        .linkage = .static,
        .root_module = b.createModule(.{
            .target = context.target,
            .optimize = context.optimize,
            .link_libc = true,
        }),
    });
    applySanitize(lib.root_module, context.sanitize);
    if (context.libc_file) |file| lib.setLibCFile(file);
    addComponentIncludes(b, lib);
    addHostSystemPaths(b, lib, context.target);
    addDirSources(lib, b, src_dir, skip);
    context.cdb.append(b.allocator, lib) catch @panic("OOM");
    return lib;
}

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    var cdb_targets: std.ArrayList(*std.Build.Step.Compile) = .empty;

    const is_windows = target.result.os.tag == .windows;

    // h2o is a posix event loop: epoll or kqueue, sys/socket.h, no Windows port. On Windows the
    // build therefore selects the fallback backend -- libuv and picohttpparser, one thread,
    // HTTP/1.1, no TLS -- which answers requests rather than merely starting up. Everywhere
    // else this defaults to on, because the fast path is what the fast servers are for.
    const enable_h2o = b.option(bool, "h2o", "Build the h2o HTTP backend; off selects the libuv+picohttpparser fallback (forced off on Windows: h2o has no Windows port)") orelse !is_windows;
    // Both databases are in by default, because which one is used is a startup decision and not
    // a build one -- Architecture.md, *DB*: PostgreSQL is the reference, SQLite is what a small
    // community installs. Turning one off is for a deployment that knows it will never see that
    // database: -Dpostgres=false is also what saves the 155 MB postgres checkout the driver is
    // compiled from, which is why that one is worth a flag at all and SQLite's 11 MB is not.
    const enable_postgres = b.option(bool, "postgres", "Build the PostgreSQL driver (libpq). Off skips a 155 MB fetch and leaves DB_TYPE=postgresql unavailable at runtime (forced off on Windows: the libpq package has no Windows port)") orelse !is_windows;
    const enable_sqlite = b.option(bool, "sqlite", "Build the SQLite driver. Off leaves DB_TYPE=sqlite unavailable at runtime") orelse true;
    const enable_tests = b.option(bool, "tests", "Build the googletest unit tests and the integration probe") orelse false;
    // The pages the backend hands out. On by default, because a server that does not serve the
    // frontend is not the product -- and off is what a machine without bun, a sanitizer run or
    // work on the C alone wants. See staticSitesSource.
    const enable_pages = b.option(bool, "pages", "Embed the frontends from publish/ and serve them. Off builds a server that answers ROUTE_NOT_IMPLEMENTED for every page (and needs no bun)") orelse true;
    const publish_root = b.option([]const u8, "publish", "Where `turbo publish` assembled the pages, relative to this build root") orelse "../publish";
    const enable_benchmarks = b.option(bool, "benchmarks", "Build the bench_* binaries") orelse false;
    const sanitize = b.option(SanitizeMode, "sanitize", "Instrument C sources: undefined_behavior (UBSan) or thread (TSan). AddressSanitizer needs the CMake build with -DFS_ENABLE_SANITIZERS=ON") orelse .off;

    if (enable_postgres and is_windows) {
        std.debug.panic("-Dpostgres=true does not build for Windows: allyourcodebase/libpq " ++
            "compiles postgres' posix src/port, and its own README claims Linux and macOS " ++
            "only. Leave it off -- the binary then carries SQLite, and CMakeLists.txt is " ++
            "where a Windows build reaches an installed libpq, which is what that file is " ++
            "for.", .{});
    }
    if (enable_h2o and is_windows) {
        std.debug.panic("-Dh2o=true does not build for Windows: h2o is a posix event loop. " ++
            "Leave it off and the binary serves over libuv and picohttpparser instead.", .{});
    }
    // Every other target cross compiles with the fast backend, `-Dtarget=aarch64-linux-musl`
    // included -- see the .libressl entry in build.zig.zon for what that took.

    const libc_file = nativeLibcFile(b, target);

    const context: Context = .{
        .b = b,
        .target = target,
        .optimize = optimize,
        .sanitize = sanitize,
        .libc_file = libc_file,
        .cdb = &cdb_targets,
    };

    // The two HTTP backends live beside each other in service-core/src; exactly one is compiled.
    const http_skip: []const []const u8 = if (enable_h2o) &.{"http_fallback.c"} else &.{"http_h2o.c"};

    const service_core = addComponent(&context, "service_core", "service-core/src", http_skip);
    const backend_core = addComponent(&context, "backend_core", "backend-core/src", &.{});
    // The migration SQL and index.json, as bytes. Generated rather than checked in, and
    // regenerated whenever the contract changes -- see migrationsSource.
    backend_core.addCSourceFile(.{ .file = migrationsSource(b), .flags = &c_flags });
    const backend = addComponent(&context, "backend", "backend/src", &.{});
    // The published frontends, as bytes. Built first when they are wanted, because the source
    // below is written at configure time out of what that run leaves in publish/.
    if (enable_pages) runPublish(b, publish_root);
    backend.addCSourceFile(.{ .file = staticSitesSource(b, publish_root, enable_pages), .flags = &c_flags });
    const federation = addComponent(&context, "federation", "federation/src", &.{});
    // dht-node's Rust half does not exist yet; src/ holds the C stand-in behind the same
    // extern "C" header. See dht-node/src/dht_node_stub.c for what changes when it lands, and
    // dht-node/Architecture.md for why it is Rust at all.
    const dht_node = addComponent(&context, "dht_node", "dht-node/src", &.{});

    // libuv is the platform layer, not an HTTP detail: the session cache's reader/writer lock,
    // the log's mutex and the thread each role runs on all come from its loop-free half. Every
    // build links it, which is why it is fetched here rather than inside the backend branch.
    // AGENTS.md section 3a holds the decision.
    const uv_dep = b.dependency("libuv", .{ .target = target, .optimize = optimize });
    const uv = uv_dep.artifact("uv");
    addHostSystemPaths(b, uv, target);
    if (libc_file) |file| uv.setLibCFile(file);

    // gradido-blockchain-core: the money arithmetic and the wire formats backend-core will be
    // written against. It vendors yyjson and links libsodium, so neither may be pinned
    // separately -- that puts a second definition of every symbol in front of the linker.
    const gbc_dep = b.dependency("blockchain_core", .{
        .target = target,
        .optimize = optimize,
        .sodium = true,
    });
    const gbc = gbc_dep.artifact("gradido_blockchain_core");
    addHostSystemPaths(b, gbc, target);
    if (libc_file) |file| gbc.setLibCFile(file);

    // libsodium, requested with the core's own options so that both get the same instance --
    // see build.zig.zon. service-core reaches for it directly because jwt.c does; the core
    // reaches for it because its crypto/ does.
    const sodium_dep = b.dependency("libsodium", .{
        .target = target,
        .optimize = optimize,
        .static = true,
        .shared = false,
    });
    const sodium = sodium_dep.artifact(if (is_windows) "libsodium-static" else "sodium");
    // libsodium reaches errno.h, so it needs the corrected libc description as much as the core
    // does -- see nativeLibcFile. Without it a build that has not got the object in its cache
    // already fails on asm/errno.h, which is how this line came to be missing for a while: the
    // Debug build kept finding one and only a change of optimize mode asked for a fresh compile.
    if (libc_file) |file| sodium.setLibCFile(file);

    // arnm carries the arena, the containers and the conversions the core is written against --
    // `arnm_result` is what a grd* call answers with, so its headers belong on the path of
    // anything that includes a core header. It was called hostmem until arnm 0.5.0 renamed
    // every symbol it has. Requested with the core's options, so that both get one instance.
    const arnm_dep = b.dependency("arnm", .{ .target = target, .optimize = optimize });

    // uv.h is included by service-core's own sources and by main.c, so the header path and the
    // library go on everything that compiles either -- a native Debian build needs
    // addMultiarchIncludeDir on each of them as well, which addComponent already does.
    // The roles are here too: bc_context carries the lock that serialises the one database
    // connection against the many loops, so uv.h reaches every translation unit that includes
    // backend_core.h.
    for ([_]*std.Build.Step.Compile{ service_core, backend_core, backend, federation }) |compile| {
        compile.linkLibrary(uv);
    }

    // The roles are on this list as well as the two libraries: a request body is JSON, and the
    // reader that parses it is arnm's -- see AGENTS.md section 3a, reach for arnm's surface. The
    // include paths follow the same rule component_includes does, which is that a header a
    // component may use is on its search path rather than argued about per target.
    for ([_]*std.Build.Step.Compile{ service_core, backend_core, backend, federation }) |compile| {
        // The core hides its crypto declarations behind this macro, so a consumer that does not
        // define it sees a different header than the one that was compiled.
        compile.root_module.addCMacro("USE_SODIUM", "1");
        compile.linkLibrary(gbc);
        compile.linkLibrary(sodium);
        compile.addIncludePath(gbc_dep.path("include"));
        // data/unit.h reaches for "r128/r128.h", which the core vendors rather than installs.
        compile.addIncludePath(gbc_dep.path("third_party"));
        compile.addIncludePath(arnm_dep.path("include"));
    }

    const exe = b.addExecutable(.{
        // The product's name, not the directory's: a binary somebody copies onto a server is
        // called what the thing is. `gradido2` is the TypeScript path's, and the suffix says
        // which of the two implementations this one is -- ../AGENTS.md, *C is the fast
        // implementation*.
        .name = "gradido2-fast",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        }),
    });
    applySanitize(exe.root_module, sanitize);
    if (libc_file) |file| exe.setLibCFile(file);
    addComponentIncludes(b, exe);
    addHostSystemPaths(b, exe, target);
    exe.addCSourceFiles(.{ .files = &.{"src/main.c"}, .flags = &c_flags });

    exe.linkLibrary(backend);
    exe.linkLibrary(federation);
    exe.linkLibrary(dht_node);
    exe.linkLibrary(backend_core);
    exe.linkLibrary(service_core);
    exe.linkLibrary(gbc);
    exe.linkLibrary(sodium);
    // main.c runs each role on a uv_thread_t.
    exe.linkLibrary(uv);

    // Both backends read from the h2o checkout: the fast one compiles libh2o out of it, the
    // fallback takes picohttpparser out of its deps/. Which is why it is fetched either way --
    // see build.zig.zon.
    const h2o_dep = b.dependency("h2o", .{});

    // TLS for h2o, and for the database. build.zig.zon, .libressl, holds why it is LibreSSL and
    // not OpenSSL; the short version is that the OpenSSL package builds for x86_64 only, and an
    // arm64 server is a thing this project intends to run on.
    //
    // h2o cannot be built without *an* OpenSSL API: <openssl/ssl.h> sits in h2o.h and
    // h2o/socket.h with no #ifdef around it, and st_h2o_socket_t carries the SSL pointer as its
    // second field, so TLS is in the socket rather than a layer over it. LibreSSL provides that
    // API and h2o knows it does -- lib/common/socket.c branches on LIBRESSL_VERSION_NUMBER in
    // four places.
    //
    // Windows gets none of this: h2o has no port there, curl gets Schannel, and libpq is not
    // built either.
    //
    // The three artifacts are asked for by name because each needs the corrected libc
    // description on a Debian host -- see nativeLibcFile. libssl links the other two, so a
    // build that only fixed the one it names would fail inside a dependency it never mentioned.
    const libressl: ?*std.Build.Step.Compile = if (is_windows) null else blk: {
        const dep = b.lazyDependency("libressl", .{ .target = target, .optimize = optimize }) orelse
            break :blk null;
        const lib = dep.artifact("ssl");
        if (libc_file) |file| {
            lib.setLibCFile(file);
            dep.artifact("crypto").setLibCFile(file);
            dep.artifact("tls").setLibCFile(file);
        }
        break :blk lib;
    };
    // Only h2o wants this, for lib/handler/compress/gzip.c. Same reasoning as libressl about
    // the pin and the options; curl is built without compression, so this has one consumer.
    const zlib: ?*std.Build.Step.Compile = if (!enable_h2o) null else blk: {
        const dep = b.lazyDependency("zlib", .{ .target = target, .optimize = optimize }) orelse
            break :blk null;
        const lib = dep.artifact("z");
        if (libc_file) |file| lib.setLibCFile(file);
        break :blk lib;
    };

    // libcurl. service-core speaks SMTP through it today and will dial HTTP(S) through it
    // later; a server that already carries one HTTP implementation has no business growing a
    // second client library beside it.
    //
    // What is switched off here is as much of the decision as what is switched on:
    //
    //   libpsl, libssh2, libidn2, nghttp2   curl looks for these on the host by default. None
    //                                       is packaged for this build, and each would put a
    //                                       system library back where the openssl move just
    //                                       took four away.
    //   zlib                                h2o already links one; a second in the same binary
    //                                       is the openssl problem in miniature. curl wants it
    //                                       only for Content-Encoding, which SMTP has no use
    //                                       for -- revisit when the HTTP client arrives.
    //   every protocol but SMTP and HTTP    curl compiles FTP, IMAP, POP3, RTSP, TFTP, SMB,
    //                                       Telnet, Gopher, DICT and MQTT unless told not to.
    //                                       None of them will ever be dialled from here, and
    //                                       all of them are parser surface in a process that
    //                                       signs transactions.
    //
    // -Dhttp-only is not what does this: it would take SMTP with it.
    const curl_dep = b.dependency("curl", .{
        .target = target,
        .optimize = optimize,
        // Not OpenSSL, and not LibreSSL either: curl's package cannot take one -- its build.zig
        // reads `// TODO BoringSSL, AWS-LC, LibreSSL, and quictls` where the choice is made, so
        // asking for OpenSSL here would fetch the x86_64-only package this build just left, and
        // put a second copy of every SSL_* symbol beside LibreSSL's while it was at it.
        //
        // mbedtls has its own API and its own symbols, so the two coexist in one process
        // without a link-order lottery -- which is exactly what two OpenSSL-API libraries
        // would be. curl pins it itself, lazily, so nothing is fetched on a build that does
        // not select it.
        //
        // This is the arrangement to revisit first when curl's package grows LibreSSL: the
        // mail client and h2o would then share one TLS library, and this file would carry one
        // less of them.
        .@"use-openssl" = false,
        .@"use-mbedtls" = !is_windows,
        .@"use-schannel" = is_windows,
        .libpsl = false,
        .libssh2 = false,
        .libidn2 = false,
        .nghttp2 = false,
        .zlib = false,
        .@"disable-dict" = true,
        .@"disable-file" = true,
        .@"disable-ftp" = true,
        .@"disable-gopher" = true,
        .@"disable-imap" = true,
        .@"disable-ldap" = true,
        .@"disable-mqtt" = true,
        .@"disable-pop3" = true,
        .@"disable-rtsp" = true,
        .@"disable-smb" = true,
        .@"disable-telnet" = true,
        .@"disable-tftp" = true,
        .@"disable-websockets" = true,
    });
    // A helper of curl's own, because the library and the command line tool are both called
    // "curl" in the build system and b.dependency().artifact() cannot tell them apart.
    const libcurl = @import("curl").artifact(curl_dep, .lib);
    if (libc_file) |file| libcurl.setLibCFile(file);
    // mail.c includes <curl/curl.h>; the path arrives with the library, as with libressl above.
    service_core.linkLibrary(libcurl);

    // mbedtls is curl's dependency and not this build's, and it is the one artifact in the
    // graph that nothing here would otherwise touch -- which matters on a Debian host, where
    // its entropy_poll.c includes sys/syscall.h and stops on asm/unistd.h without the corrected
    // libc description. See nativeLibcFile.
    //
    // Reached through curl's own builder with exactly the arguments curl passes it, so what
    // comes back is the instance curl linked rather than a second one built beside it.
    // `.threading` is curl's `!(single_threaded orelse false)`, and this build never passes
    // -Dsingle-threaded.
    if (libc_file) |file| {
        if (!is_windows) {
            if (curl_dep.builder.lazyDependency("mbedtls", .{
                .target = target,
                .optimize = optimize,
                .threading = true,
            })) |dep| dep.artifact("mbedtls").setLibCFile(file);
        }
    }

    // The two database drivers. Both are compiled in where the build allows it, because which
    // one a community runs is decided in its environment at startup and not here -- DB_TYPE,
    // the same variable the TypeScript path reads. A driver the build left out is not an error
    // until something asks for that database; service-core answers SC_ERR_UNAVAILABLE and says
    // which build option would have provided it.
    //
    // service-core's two driver files see these headers, and so do backend-core's repositories:
    // there is deliberately no query surface both dialects implement, so a statement is written
    // against the driver in a file that already knows which dialect it is in -- Architecture.md,
    // *Databases*. Everything above those files is written against service_core/db.h, which
    // carries no driver type.
    if (enable_postgres) {
        // Null only on the pass that fetches a lazy dependency; nothing is built on that pass.
        if (b.lazyDependency("libpq", .{
            .target = target,
            .optimize = optimize,
            // The same LibreSSL h2o is built against, and that is not a coincidence: this
            // package pins allyourcodebase/libressl at 8c63fad with the hash build.zig.zon
            // names, so the graph memoizes one instance and the process carries one TLS
            // library rather than two that export the same symbols.
            .ssl = .LibreSSL,
            // Wire compression against the database, and the same argument as the ssl one: the
            // package pins a zlib of its own, h2o already links ours, and PostgreSQL only
            // compresses what libpq negotiates -- which is nothing this build asks for.
            .@"disable-zlib" = true,
            .@"disable-zstd" = true,
        })) |dep| {
            const libpq = dep.artifact("pq");
            // libpq's sources include errno.h, so it needs the corrected libc description for
            // the same reason libsodium does -- see nativeLibcFile. Without it a Debian host
            // stops with 88 errors on asm/errno.h inside the dependency.
            if (libc_file) |file| libpq.setLibCFile(file);
            for ([_]*std.Build.Step.Compile{ service_core, backend_core }) |compile| {
                compile.linkLibrary(libpq);
                compile.root_module.addCMacro("SC_DB_WITH_POSTGRESQL", "1");
            }
        }
    }

    if (enable_sqlite) {
        const sqlite = buildSqlite(b, target, optimize, sanitize);
        if (libc_file) |file| sqlite.setLibCFile(file);
        for ([_]*std.Build.Step.Compile{ service_core, backend_core }) |compile| {
            compile.linkLibrary(sqlite);
            compile.root_module.addCMacro("SC_DB_WITH_SQLITE", "1");
        }
    }

    // What every binary serving HTTP has to link. Collected rather than applied inline, because
    // the integration probe is a second such binary and the two must not drift apart.
    var http_backend_lib: ?*std.Build.Step.Compile = null;
    // h2o's system dependencies belong on the executable rather than in a static archive:
    // packing shared objects into one makes lld object, and lld has a point.
    //
    // This was `ssl, crypto, z, m` while three of the four came from the host. Only libm is
    // left, and it is the one no package replaces.
    const http_system_libs: []const []const u8 = if (enable_h2o) &.{"m"} else &.{};

    // Only service-core includes the selected backend's headers. Everything above it is written
    // against service_core/http.h and does not know which one answered.
    if (enable_h2o) {
        service_core.root_module.addCMacro("H2O_USE_LIBUV", "0");
        for (h2o_include_dirs) |dir| service_core.addIncludePath(h2o_dep.path(dir));
        // http_h2o.c includes h2o.h, which includes <openssl/ssl.h>. The header path for that
        // arrives with the library rather than as an addIncludePath, because the libressl
        // package installs its include directory and linkLibrary carries it along.
        if (libressl) |ssl| service_core.linkLibrary(ssl);

        // Null only on a pass that is fetching a lazy dependency, and such a pass builds
        // nothing -- the build re-runs afterwards with both in hand.
        if (libressl != null and zlib != null) {
            const h2o = buildH2o(b, h2o_dep, target, optimize, libressl.?, zlib.?);
            if (libc_file) |file| h2o.setLibCFile(file);
            http_backend_lib = h2o;
        }
    } else {
        // The platform layer is already linked; the fallback backend only adds the parser.
        http_backend_lib = uv;
        // picohttpparser, out of the same h2o checkout the other backend is built from. It is
        // two files with no build system of their own, so they are compiled straight into
        // service-core -- and only picohttpparser.c is named, because upstream ships a bench.c
        // and a test.c beside it and each of those carries a main().
        service_core.addIncludePath(h2o_dep.path("deps/picohttpparser"));
        service_core.addCSourceFiles(.{
            .root = h2o_dep.path("."),
            .files = &.{"deps/picohttpparser/picohttpparser.c"},
            // The standard is named for the same reason as in c_flags, so both builds compile
            // the same language. The warning flags are absent: upstream is not ours to keep
            // clean under them.
            .flags = &.{"-std=gnu11"},
        });
    }
    linkHttpBackend(exe, http_backend_lib, http_system_libs);

    cdb_targets.append(b.allocator, exe) catch @panic("OOM");
    b.installArtifact(exe);

    if (enable_benchmarks) {
        // Linked exactly the way the server is, which is the point of building them here rather
        // than by hand: what a JWT costs depends on which libsodium is underneath, and the one
        // this build pins is not the one on the system.
        //
        // Built at whatever -Doptimize says, and a Debug build measures Debug. Use
        // `--release=fast` for a number worth quoting.
        const benchmarks = [_][]const u8{"bench_jwt"};
        for (benchmarks) |name| {
            const bench = b.addExecutable(.{
                .name = name,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                }),
            });
            applySanitize(bench.root_module, sanitize);
            if (libc_file) |file| bench.setLibCFile(file);
            addComponentIncludes(b, bench);
            addHostSystemPaths(b, bench, target);
            bench.addIncludePath(arnm_dep.path("include"));
            bench.addCSourceFiles(.{
                .files = &.{b.fmt("benchmarks/{s}.c", .{name})},
                .flags = &c_flags,
            });
            bench.linkLibrary(service_core);
            bench.linkLibrary(gbc);
            bench.linkLibrary(sodium);
            cdb_targets.append(b.allocator, bench) catch @panic("OOM");
            b.installArtifact(bench);
        }
    }

    // `zig build test` builds the unit tests and runs them. Without -Dtests it has nothing to
    // do and says so, rather than silently succeeding on an empty set.
    const test_step = b.step("test", "Run the unit tests (needs -Dtests)");

    if (enable_tests) {
        // Unit tests live beside the component they test rather than in one tree at the root,
        // which is where arnm and gradido-blockchain-core keep theirs. Those are one library
        // each; this is five, and a test binary that links one component and sees only that
        // component's include directory is what proves the header carries its own dependencies.
        // A shared test tree with all five paths on it can never fail that way.
        const unit_tests = [_]UnitTest{
            .{ .name = "test_cache", .dir = "service-core/tests", .src = "test_cache.cpp", .lib = service_core },
            .{ .name = "test_mail", .dir = "service-core/tests", .src = "test_mail.cpp", .lib = service_core },
            .{ .name = "test_db", .dir = "service-core/tests", .src = "test_db.cpp", .lib = service_core },
            .{ .name = "test_jwt", .dir = "service-core/tests", .src = "test_jwt.cpp", .lib = service_core },
            .{ .name = "test_migrations", .dir = "backend-core/tests", .src = "test_migrations.cpp", .lib = backend_core, .deps = &.{service_core}, .includes = &.{"service-core/include"} },
            .{ .name = "test_user", .dir = "backend-core/tests", .src = "test_user.cpp", .lib = backend_core, .deps = &.{service_core}, .includes = &.{"service-core/include"} },
            .{ .name = "test_field_rules", .dir = "backend/tests", .src = "test_field_rules.cpp", .sources = &.{"backend/src/field_rules.c"}, .includes = &.{"backend/src"} },
            .{ .name = "test_static_sites", .dir = "backend/tests", .src = "test_static_sites.cpp", .sources = &.{"backend/src/static_sites.c"}, .includes = &.{"service-core/include"} },
        };

        for (unit_tests) |unit_test| {
            const test_exe = b.addExecutable(.{
                .name = unit_test.name,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                    .link_libcpp = true,
                }),
            });
            applySanitize(test_exe.root_module, sanitize);
            if (libc_file) |file| test_exe.setLibCFile(file);
            // Only the component under test is on the include path, plus whatever that
            // component itself is written against. That is the point -- see UnitTest.includes.
            test_exe.addIncludePath(b.path(b.fmt("{s}/include", .{std.fs.path.dirname(unit_test.dir).?})));
            for (unit_test.includes) |dir| test_exe.addIncludePath(b.path(dir));
            test_exe.addCSourceFiles(.{
                .files = &.{b.fmt("{s}/{s}", .{ unit_test.dir, unit_test.src })},
                // The googletest macros do not compile clean under our flags and are not ours
                // to fix, so the test sources get the standard and nothing else.
                .flags = &.{"-std=gnu++17"},
            });
            if (unit_test.sources.len != 0)
                test_exe.addCSourceFiles(.{ .files = unit_test.sources, .flags = &c_flags });
            if (unit_test.lib) |lib| test_exe.linkLibrary(lib);
            for (unit_test.deps) |dep| test_exe.linkLibrary(dep);
            addHostSystemPaths(b, test_exe, target);
            if (b.lazyDependency("googletest", .{ .target = target, .optimize = optimize })) |dep| {
                const gtest = dep.artifact("gtest");
                // googletest reaches errno.h through libc++, so it needs the same libc
                // description as everything else this build touches -- see nativeLibcFile.
                if (libc_file) |file| gtest.setLibCFile(file);
                test_exe.linkLibrary(gtest);
            }
            cdb_targets.append(b.allocator, test_exe) catch @panic("OOM");
            b.installArtifact(test_exe);

            const run_test = b.addRunArtifact(test_exe);
            run_test.skip_foreign_checks = true;
            test_step.dependOn(&run_test.step);
        }

        // The contract runners: `contracts/test-vectors/<subject>.json`, run against this
        // implementation. The TypeScript half of the same files is `packages/contract-tests`,
        // and neither half is the authority -- the file is. See tests/contract/vectors.hpp.
        //
        // Not one of the unit tests above, and the include path is why: a runner has to read the
        // vector file, which means arnm's header, and a component test that sees a header its
        // component does not carry stops proving what the loop above exists to prove. So these
        // are built here instead, with arnm and the directory the vectors live in.
        const contract_tests = [_][]const u8{"test_jwt_contract"};
        for (contract_tests) |name| {
            const contract_exe = b.addExecutable(.{
                .name = name,
                .root_module = b.createModule(.{
                    .target = target,
                    .optimize = optimize,
                    .link_libc = true,
                    .link_libcpp = true,
                }),
            });
            applySanitize(contract_exe.root_module, sanitize);
            if (libc_file) |file| contract_exe.setLibCFile(file);
            contract_exe.addIncludePath(b.path("service-core/include"));
            contract_exe.addIncludePath(b.path("tests/contract"));
            contract_exe.addIncludePath(arnm_dep.path("include"));
            // Where the vectors are, compiled in, so the binary finds them without a working
            // directory. GRADIDO_CONTRACT_VECTORS_DIR overrides it for one that was moved.
            contract_exe.root_module.addCMacro(
                "FS_CONTRACT_VECTORS_DIR",
                b.fmt("\"{s}\"", .{b.pathFromRoot("../contracts/test-vectors")}),
            );
            contract_exe.addCSourceFiles(.{
                .files = &.{b.fmt("tests/contract/{s}.cpp", .{name})},
                // As for the unit tests: googletest's macros do not compile clean under our
                // flags and are not ours to fix.
                .flags = &.{"-std=gnu++17"},
            });
            contract_exe.linkLibrary(service_core);
            contract_exe.linkLibrary(gbc);
            contract_exe.linkLibrary(sodium);
            if (b.lazyDependency("googletest", .{ .target = target, .optimize = optimize })) |dep| {
                const gtest = dep.artifact("gtest");
                if (libc_file) |file| gtest.setLibCFile(file);
                contract_exe.linkLibrary(gtest);
            }
            cdb_targets.append(b.allocator, contract_exe) catch @panic("OOM");
            b.installArtifact(contract_exe);

            const run_contract = b.addRunArtifact(contract_exe);
            run_contract.skip_foreign_checks = true;
            test_step.dependOn(&run_contract.step);
        }

        // The server the integration suite drives. It is a second consumer of
        // service_core/http.h and not one of the three roles -- tests/integration/probe says
        // why the echo routes are not in backend or federation. Driving it is `bun test` in
        // tests/integration, which is not wired into this step: zig does not run bun, and a
        // build step that shells out to another toolchain fails for reasons that have nothing
        // to do with the build.
        const probe = b.addExecutable(.{
            .name = "http-probe",
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
                .link_libc = true,
            }),
        });
        applySanitize(probe.root_module, sanitize);
        if (libc_file) |file| probe.setLibCFile(file);
        addComponentIncludes(b, probe);
        addHostSystemPaths(b, probe, target);
        probe.addCSourceFiles(.{
            .files = &.{"tests/integration/probe/http_probe.c"},
            .flags = &c_flags,
        });
        probe.linkLibrary(service_core);
        // uv.h directly: the probe owns the worker thread behind /defer, which is what makes
        // sc_http_defer testable at all. linkLibrary does not carry a dependency's header path
        // through service_core, so it is asked for here.
        probe.linkLibrary(uv);
        probe.linkLibrary(gbc);
        probe.linkLibrary(sodium);
        linkHttpBackend(probe, http_backend_lib, http_system_libs);
        cdb_targets.append(b.allocator, probe) catch @panic("OOM");
        b.installArtifact(probe);
    } else {
        const explain = b.addFail("no tests were built; add -Dtests");
        test_step.dependOn(&explain.step);
    }

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| run_cmd.addArgs(args);
    const run_step = b.step("run", "Run gradido2-fast; pass roles after --, e.g. -- --backend --dht-node");
    run_step.dependOn(&run_cmd.step);

    const cdb_slice = cdb_targets.toOwnedSlice(b.allocator) catch @panic("OOM");
    const cdb_step = zcc.createStep(b, "cdb", cdb_slice);
    for (cdb_slice) |t| cdb_step.dependOn(&t.step);
    // Regenerate compile_commands.json on every build, not only on `zig build cdb`
    b.getInstallStep().dependOn(cdb_step);
}
