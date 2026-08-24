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
fn buildH2o(
    b: *std.Build,
    dep: *std.Build.Dependency,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
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
    lib: *std.Build.Step.Compile,
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
    const enable_tests = b.option(bool, "tests", "Build the googletest unit tests and the integration probe") orelse false;
    const enable_benchmarks = b.option(bool, "benchmarks", "Build the bench_* binaries") orelse false;
    const sanitize = b.option(SanitizeMode, "sanitize", "Instrument C sources: undefined_behavior (UBSan) or thread (TSan). AddressSanitizer needs the CMake build with -DFS_ENABLE_SANITIZERS=ON") orelse .off;

    if (enable_h2o and is_windows) {
        std.debug.panic("-Dh2o=true does not build for Windows: h2o is a posix event loop. " ++
            "Leave it off and the binary serves over libuv and picohttpparser instead.", .{});
    }
    // h2o includes <openssl/ssl.h> and links ssl, crypto and z, none of which this build
    // provides and all of which come from the host. For the host itself that is fine -- named
    // or native, addHostSystemPaths finds them. For any other target they are the wrong
    // machine's, and what zig says about it is 78 lines of 'openssl/ssl.h' file not found.
    if (enable_h2o and !targetIsHost(target)) {
        std.debug.panic(
            "-Dh2o=true cannot cross compile to {s}-{s}-{s}: h2o needs the host's OpenSSL and " ++
                "zlib, and this build does not carry them. Add -Dh2o=false for the " ++
                "libuv+picohttpparser backend, which needs nothing from the host.",
            .{ @tagName(target.result.cpu.arch), @tagName(target.result.os.tag), @tagName(target.result.abi) },
        );
    }

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
    const backend = addComponent(&context, "backend", "backend/src", &.{});
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
    // written against, and -- today -- the two libraries service-core reaches into. It vendors
    // yyjson and links libsodium, so pinning either separately would put a second definition of
    // every yyjson_* symbol in front of the linker and leave which one wins to link order.
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
    for ([_]*std.Build.Step.Compile{ service_core, backend_core }) |compile| {
        compile.linkLibrary(uv);
    }

    for ([_]*std.Build.Step.Compile{ service_core, backend_core }) |compile| {
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
        .name = "fast-servers",
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

    // What every binary serving HTTP has to link. Collected rather than applied inline, because
    // the integration probe is a second such binary and the two must not drift apart.
    var http_backend_lib: ?*std.Build.Step.Compile = null;
    // h2o's system dependencies belong on the executable rather than in a static archive:
    // packing shared objects into one makes lld object, and lld has a point.
    const http_system_libs: []const []const u8 =
        if (enable_h2o) &.{ "ssl", "crypto", "z", "m" } else &.{};

    // Only service-core includes the selected backend's headers. Everything above it is written
    // against service_core/http.h and does not know which one answered.
    if (enable_h2o) {
        const h2o = buildH2o(b, h2o_dep, target, optimize);
        if (libc_file) |file| h2o.setLibCFile(file);

        service_core.root_module.addCMacro("H2O_USE_LIBUV", "0");
        for (h2o_include_dirs) |dir| service_core.addIncludePath(h2o_dep.path(dir));

        http_backend_lib = h2o;
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
            // Only the component under test is on the include path. That is the point.
            test_exe.addIncludePath(b.path(b.fmt("{s}/include", .{std.fs.path.dirname(unit_test.dir).?})));
            test_exe.addCSourceFiles(.{
                .files = &.{b.fmt("{s}/{s}", .{ unit_test.dir, unit_test.src })},
                // The googletest macros do not compile clean under our flags and are not ours
                // to fix, so the test sources get the standard and nothing else.
                .flags = &.{"-std=gnu++17"},
            });
            test_exe.linkLibrary(unit_test.lib);
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
    const run_step = b.step("run", "Run fast-servers; pass roles after --, e.g. -- --backend --dht-node");
    run_step.dependOn(&run_cmd.step);

    const cdb_slice = cdb_targets.toOwnedSlice(b.allocator) catch @panic("OOM");
    const cdb_step = zcc.createStep(b, "cdb", cdb_slice);
    for (cdb_slice) |t| cdb_step.dependOn(&t.step);
    // Regenerate compile_commands.json on every build, not only on `zig build cdb`
    b.getInstallStep().dependOn(cdb_step);
}
