//! Builds two things out of the same C:
//!
//!   email_native.node   the Node-API addon -- renderer + service-core's SMTP mailer
//!   dump / bench        the standalone C-server path, renderer only
//!
//! Before any of it, two codegen steps run into the zig cache:
//!
//!   node tools/extract_mjml.mjs  ->  ir.json                (mjml + gettext, build time only)
//!   node tools/gen_c.mjs    ->  service_core/email/templates.h + templates.c
//!
//! Every mjml, po and PNG file under templates/ and po/ is registered as an input,
//! so the codegen re-runs exactly when a template changes and is skipped otherwise -- zig
//! hashes contents, not mtimes, so a bare `touch` triggers nothing.
//!
//! The default step also runs `check`: the C binary renders all 810 documents and they
//! are compared against tests/__snapshots__, the checked-in extractor output. A build whose
//! tables no longer match the templates fails there rather than in a mail.
//!
//! The two generated files are also installed into the output directory, because
//! fast-servers compiles them rather than generating them: `scripts/sync-fast-servers.ts`
//! copies them into `service-core/` after the build, which is what keeps `zig build` in
//! fast-servers free of node and mjml. That install hangs off the default step, so a plain
//! `c-cpp-zig-build` produces both the addon and the files to copy.
//!
//!   npx czb                                        addon, generated C, and the check
//!   npx czb --optimize fast                        the same, ReleaseFast
//!   zig build -Daddon=false exe                    the executables, no Node headers needed
//!   zig build -Daddon=false check                  the check on its own
//!   zig build -Daddon=false bench -Doptimize=ReleaseFast
//!   ... -Dfast-servers=<path>                      another checkout of the C servers
//!
//! Node is needed for the codegen (mjml is a JS library) and, for the addon, its
//! headers -- which `c-cpp-zig-build` downloads and passes as -Dnode-headers.

const std = @import("std");
const czb = @import("c_cpp_zig_build");

const cflags = [_][]const u8{ "-std=gnu11", "-Wall", "-Wextra" };

/// The MJML templates and the message catalogues, the single source of truth for
/// both implementations from here on.
const templates_dir = "templates";
/// The message catalogues, gettext .po -- one msgid per English source string, the
/// same convention packages/frontend uses. locales/*.json is legacy's key-based
/// form and is kept only for tools/extract.mjs, the importer; nothing here reads it.
const po_dir = "po";

/// The extractor output, checked in: what `check` holds the C binary against. Written by
/// `bun run snapshots:update`, see tools/snapshots.mjs.
const snapshots_dir = "tests/__snapshots__";

pub fn build(b: *std.Build) !void {
    const fast_servers = b.option(
        []const u8,
        "fast-servers",
        "path to gradido2's fast-servers (default: ../../fast-servers)",
    ) orelse "../../fast-servers";

    // The addon needs Node headers; the executables do not. Turning it off is what
    // lets `zig build check` run in CI without the npm helper.
    const want_addon = b.option(bool, "addon", "build the Node-API addon (default: true)") orelse true;

    // Off leaves out the sending half and everything under it -- curl and mbedtls. The point
    // of the switch is that the difference is measurable.
    const want_mailer = b.option(bool, "mailer", "include the SMTP mailer (default: true)") orelse true;
    // Only to put a number on what TLS costs. A build with -Dtls=false speaks SMTP
    // in the clear and has no business anywhere near a relay.
    const want_tls = b.option(bool, "tls", "build curl with mbedtls (default: true)") orelse true;
    const want_trim = b.option(bool, "trim", "cut mbedtls and curl down to an SMTP client (default: true)") orelse true;
    // Two further cuts, off by default because each gives something up.
    const want_http = b.option(bool, "http", "keep curl's HTTP support (default: true)") orelse true;
    const want_tls13 = b.option(bool, "tls13", "keep TLS 1.3 (default: true)") orelse true;

    // ---- 1) mjml + gettext -> intermediate format ----------------------------
    const extract = b.addSystemCommand(&.{ "node", "tools/extract_mjml.mjs" });
    extract.addArgs(&.{ "--templates", templates_dir, "--po", po_dir });
    extract.addArg("--out");
    const ir_dir = extract.addOutputDirectoryArg("ir");
    extract.addFileInput(b.path("tools/extract_mjml.mjs"));
    extract.addFileInput(b.path("tools/branches.mjs"));
    extract.addFileInput(b.path("tools/manifest.mjs"));
    try addTreeAsInput(b, extract, templates_dir);
    try addTreeAsInput(b, extract, po_dir);

    // ---- 2) intermediate format -> C ---------------------------------------
    // Reads the templates once more for the inline PNGs (cid: attachments).
    const gen = b.addSystemCommand(&.{ "node", "tools/gen_c.mjs" });
    gen.addArgs(&.{ "--templates", templates_dir });
    gen.addArg("--ir");
    gen.addFileArg(ir_dir.path(b, "ir.json"));
    gen.addArg("--out");
    const gen_dir = gen.addOutputDirectoryArg("gen");
    gen.addFileInput(b.path("tools/gen_c.mjs"));
    gen.addFileInput(b.path("tools/manifest.mjs"));
    try addTreeAsInput(b, gen, templates_dir);

    // ---- 3) the two generated files, on disk for fast-servers ----------------
    // Part of the default step: `npx czb` is what the turbo build runs, and the copy
    // into fast-servers happens straight after it.
    const install_gen_h = b.addInstallFileWithDir(
        gen_dir.path(b, "service_core/email/templates.h"),
        .prefix,
        "gen/service_core/email/templates.h",
    );
    const install_gen_c =
        b.addInstallFileWithDir(gen_dir.path(b, "templates.c"), .prefix, "gen/templates.c");
    b.getInstallStep().dependOn(&install_gen_h.step);
    b.getInstallStep().dependOn(&install_gen_c.step);

    const gen_step = b.step("gen", "Write the generated C into <prefix>/gen, for fast-servers");
    gen_step.dependOn(&install_gen_h.step);
    gen_step.dependOn(&install_gen_c.step);

    // ---- 4) the addon --------------------------------------------------------
    var target: std.Build.ResolvedTarget = undefined;
    var optimize: std.builtin.OptimizeMode = undefined;

    if (want_addon) {
        // src/ holds the runtime core and napi/ the bindings; both are picked up by
        // default. The generated translation unit lives in the cache and is added
        // by hand below.
        const addon = try czb.addNodeAddon(b, .{ .name = "email_native" });
        target = addon.target;
        optimize = addon.optimize;

        // The public header is include/service_core/email/render.h -- the path it has in
        // fast-servers, so there is one include spelling and not two.
        addon.addIncludePath("include");
        for (addon.compiles) |compile| {
            // See napi/exports.map: this is what keeps the statically linked libuv
            // from being preempted by the host's, and it is why the addon runs on
            // Bun at all.
            compile.setVersionScript(b.path("napi/exports.map"));
            compile.addIncludePath(gen_dir);
            compile.addCSourceFile(.{ .file = gen_dir.path(b, "templates.c"), .flags = &cflags });
        }
        if (want_mailer) {
            addon.addDefine("GE_WITH_MAILER", "1");
            addMailer(b, addon.compiles, fast_servers, target, optimize, want_tls, want_trim, want_http, want_tls13);
        }
    } else {
        target = b.standardTargetOptions(.{});
        optimize = b.standardOptimizeOption(.{});
    }

    // ---- 5) the standalone C-server path ------------------------------------
    const dump = addExe(b, "dump", "tools/dump.c", gen_dir, target, optimize);
    const bench = addExe(b, "bench", "tools/bench.c", gen_dir, target, optimize);

    // Not installed by default: `npx czb` should produce the addon and the generated
    // C, and nothing else.
    const exe_step = b.step("exe", "Build and install dump and bench");
    for ([_]*std.Build.Step.Compile{ dump, bench }) |c| {
        const inst = b.addInstallArtifact(c, .{});
        exe_step.dependOn(&inst.step);
    }

    const run_bench = b.addRunArtifact(bench);
    b.step("bench", "Buffer size probe and throughput (use -Doptimize=ReleaseFast)")
        .dependOn(&run_bench.step);

    // ---- check --------------------------------------------------------------
    // dump renders everything in the binary with the fixture values; verify.mjs
    // compares that against tests/__snapshots__, the checked-in extractor output.
    // No mjml here -- what keeps the snapshots equal to the templates is
    // tests/snapshots.test.mjs, and the two halves together are what backs
    // "the C renders what the templates say".
    const run_dump = b.addRunArtifact(dump);
    const c_out = run_dump.addOutputDirectoryArg("c");

    const verify = b.addSystemCommand(&.{ "node", "tools/verify.mjs" });
    verify.addArg("--c");
    verify.addDirectoryArg(c_out);
    verify.addFileInput(b.path("tools/verify.mjs"));
    verify.addFileInput(b.path("tools/manifest.mjs"));
    try addTreeAsInput(b, verify, snapshots_dir);
    verify.expectExitCode(0);

    // Part of the default step, so every build proves it before the generated C is
    // copied into fast-servers: a build that cannot show the templates and the tables
    // still agree has nothing worth copying. It costs one dump run and a file compare.
    b.getInstallStep().dependOn(&verify.step);

    b.step("check", "Prove the C output equals the checked-in snapshots")
        .dependOn(&verify.step);
}

/// service-core's mail, minus its worker pool: email/message.c formats the RFC 5322 bytes and
/// email/transport.c sends one message over one curl session. Both are compiled straight out of
/// the fast-servers checkout rather than vendored, so the addon and the C server cannot send
/// different mail -- and neither of them knows about threads, which is why nothing below links
/// libuv or arnm. The queue and the workers stay in fast-servers, where the load is.
fn addMailer(
    b: *std.Build,
    compiles: []const *std.Build.Step.Compile,
    fast_servers: []const u8,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    tls: bool,
    trim: bool,
    http: bool,
    tls13: bool,
) void {
    const root: std.Build.LazyPath = .{ .cwd_relative = b.pathJoin(&.{ fast_servers, "service-core" }) };

    // Every protocol but SMTP is switched off, and so are the four system libraries
    // curl reaches for by default. Same options fast-servers passes, so both builds
    // resolve to one package in the graph.
    const curl_dep = b.dependency("curl", .{
        .target = target,
        .optimize = optimize,
        .@"use-openssl" = false,
        .@"use-mbedtls" = tls,
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

        // mail.c's whole curl surface is curl_easy_{init,setopt,perform,reset,
        // cleanup,strerror}, curl_slist_append and curl_global_*. No getinfo, no
        // mime, no multi handle. So everything below is reachable by nothing.
        //
        // The auth mechanisms are the one judgement call: PLAIN and LOGIN are not
        // removable and are what a relay with a username and a password uses over
        // TLS. Dropping these gives up CRAM-MD5 and DIGEST-MD5 (digest), NTLM,
        // GSSAPI (kerberos) and XOAUTH2/OAUTHBEARER (bearer) -- the last of which
        // would matter if a Gmail account were ever the relay.
        .@"disable-ntlm" = trim,
        .@"disable-negotiate-auth" = trim,
        .@"disable-kerberos-auth" = trim,
        .@"disable-digest-auth" = trim,
        .@"disable-bearer-auth" = trim,
        .@"disable-aws" = trim,
        .@"disable-srp" = trim,

        .@"disable-cookies" = trim,
        .@"disable-altsvc" = trim,
        .@"disable-doh" = trim,
        .@"disable-netrc" = trim,
        // form-api is not a separate switch once mime is off -- curl's package
        // refuses the option rather than ignoring it.
        .@"disable-mime" = trim,
        .@"disable-headers-api" = trim,
        .@"disable-getoptions" = trim,
        .@"disable-libcurl-option" = trim,
        .@"disable-parsedate" = trim,
        .@"disable-shuffle-dns" = trim,
        .@"disable-bindlocal" = trim,
        .@"disable-ipfs" = trim,
        .@"disable-progress-meter" = trim,
        // Kept on purpose: curl_easy_strerror feeds mail.c's error log, and a
        // build that can only say "error 56" is a bad trade for a few KB.
        .@"disable-verbose-strings" = false,

        // HTTP is not what this addon dials. fast-servers wants it, which is why
        // it is a switch rather than a constant.
        .@"disable-http" = trim and !http,
    });
    // The library and the command line tool are both called "curl" in that build
    // system, so the package hands out a helper to tell them apart.
    const libcurl = @import("curl").artifact(curl_dep, .lib);

    // curl's package hands mbedtls `.threading = true`, and sets MBEDTLS_THREADING_C
    // on the mbedtls artifact ONLY. curl embeds mbedtls_entropy_context and
    // mbedtls_ctr_drbg_context by value (lib/vtls/mbedtls.c), and both grow a
    // mbedtls_threading_mutex_t member under that macro -- so the library locks a
    // mutex past the end of the struct curl allocated. It aborts on the first TLS
    // handshake with
    //
    //   pthread_mutex_lock.c: Assertion `mutex->__data.__owner == 0' failed
    //
    // Defining it for curl as well is what makes the two agree. This is not about
    // the trim below; without it, no smtps:// connection works at all.
    if (tls) {
        libcurl.root_module.addCMacro("MBEDTLS_THREADING_C", "");
        libcurl.root_module.addCMacro("MBEDTLS_THREADING_PTHREAD", "");
    }

    // Trim mbedtls down to what an SMTP client needs -- see tls/gradido_mbedtls_config.h.
    //
    // The macro goes on BOTH artifacts, and that is not belt and braces: curl's
    // lib/vtls/mbedtls.c includes mbedtls headers, and mbedtls_ssl_context changes
    // size with this config. Two different views of that struct in one process link
    // cleanly and crash later.
    //
    // The dependency is reached through curl's own builder with exactly the
    // arguments curl passes it, so this configures the instance curl linked rather
    // than a second one built beside it.
    if (tls and trim) {
        const cfg = b.path("tls");
        libcurl.root_module.addCMacro("MBEDTLS_USER_CONFIG_FILE", "\"gradido_mbedtls_config.h\"");
        libcurl.root_module.addIncludePath(cfg);
        if (!tls13) libcurl.root_module.addCMacro("GE_TLS12_ONLY", "1");
        if (curl_dep.builder.lazyDependency("mbedtls", .{
            .target = target,
            .optimize = optimize,
            .threading = true,
        })) |dep| {
            const art = dep.artifact("mbedtls");
            art.root_module.addCMacro("MBEDTLS_USER_CONFIG_FILE", "\"gradido_mbedtls_config.h\"");
            art.root_module.addIncludePath(cfg);
            if (!tls13) art.root_module.addCMacro("GE_TLS12_ONLY", "1");
        }
    }

    for (compiles) |compile| {
        compile.addIncludePath(root.path(b, "include"));
        compile.addCSourceFiles(.{
            .root = root,
            // status.c is sc_status_str; atomic.c is the compare-and-swap that stands in for
            // uv_once in sc_mail_global_init(). log.c is deliberately absent: message.c and
            // transport.c report through return values, and the log is where libuv came in.
            .files = &.{ "src/email/message.c", "src/email/transport.c", "src/status.c",
                         "src/atomic.c" },
            .flags = &cflags,
        });
        compile.linkLibrary(libcurl);
    }
}

fn addExe(
    b: *std.Build,
    name: []const u8,
    root_c: []const u8,
    gen_dir: std.Build.LazyPath,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) *std.Build.Step.Compile {
    const mod = b.createModule(.{ .target = target, .optimize = optimize, .link_libc = true });
    mod.addIncludePath(b.path("include"));
    mod.addIncludePath(gen_dir);
    mod.addCSourceFiles(.{ .files = &.{ root_c, "src/render.c" }, .flags = &cflags });
    mod.addCSourceFile(.{ .file = gen_dir.path(b, "templates.c"), .flags = &cflags });
    return b.addExecutable(.{ .name = name, .root_module = mod });
}

/// Registers every file under `dir` as an input of `run`, so the step re-runs when a
/// template, a locale catalogue or an inline image changes -- and only then.
fn addTreeAsInput(b: *std.Build, run: *std.Build.Step.Run, dir_path: []const u8) !void {
    var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch |err| {
        std.debug.print("build.zig: cannot open {s}: {s}\n", .{ dir_path, @errorName(err) });
        return err;
    };
    defer dir.close();

    var walker = try dir.walk(b.allocator);
    defer walker.deinit();
    while (try walker.next()) |entry| {
        if (entry.kind != .file) continue;
        run.addFileInput(b.path(b.pathJoin(&.{ dir_path, entry.path })));
    }
}
