const std = @import("std");
const czb = @import("c_cpp_zig_build");

pub fn build(b: *std.Build) !void {
    // Compiles everything under napi/, puts include/ and third_party/ on the header
    // search path, and installs shared_native.node into the output directory. See the
    // c-cpp-zig-build README for the options.
    const addon = try czb.addNodeAddon(b, .{ .name = "shared_native" });

    // The crypto half of the core - signing, key derivation, the generic hash, base64 -
    // is compiled only when libsodium is there, and the headers hide the declarations
    // behind the same macro. Both sides have to agree, so the dependency is built with
    // sodium and the addon defines the macro for its own translation units.
    addon.addDefine("USE_SODIUM", "1");

    const core = addon.dependency("blockchain_core", .{ .sodium = true });
    // arnm carries what the core used to keep in utils/: the arena allocator, the
    // monotonic timer, the duration and hex/uuid conversions. It is a package of its
    // own, and the napi layer includes its headers directly. It was called hostmem
    // until arnm 0.5.0, which renamed every symbol -- so the prefix in napi/ is arnm_
    // and the include path is arnm/, and the core's public headers say the same.
    const arnm = addon.dependency("arnm", .{});

    for (addon.compiles) |compile| {
        compile.linkLibrary(core.artifact("gradido_blockchain_core"));
        compile.addIncludePath(core.path("include"));
        // data/unit.h reaches for "r128/r128.h", which the core vendors rather than installs,
        // so a consumer of its public headers needs third_party/ on the search path as well.
        compile.addIncludePath(core.path("third_party"));
        compile.addIncludePath(arnm.path("include"));
    }
}
