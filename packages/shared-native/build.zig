const std = @import("std");
const czb = @import("c_cpp_zig_build");

pub fn build(b: *std.Build) !void {
    // Compiles everything under src/ and napi/, puts include/ and third_party/
    // on the header search path, and installs shared_native.node into the
    // output directory. See the c-cpp-zig-build README for the options.
    const addon = try czb.addNodeAddon(b, .{ .name = "shared_native" });
    addon.linkDependency("blockchain_core", "gradido_blockchain_core"); 
}
