const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{ .preferred_optimize_mode = .ReleaseFast });

    const exe = b.addExecutable(.{
        .name = "explorer",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    
    exe.addIncludePath(b.path("src"));
    exe.addIncludePath(b.path("vendor/gdstk/include"));
    exe.addIncludePath(b.path("vendor/gdstk/external"));
    exe.addIncludePath(b.path("vendor/miniz"));
    exe.addIncludePath(b.path("src/qhull_stub"));

    exe.addCSourceFiles(.{
        .files = &.{
            "src/main.cpp",
            "src/wasm_allocator.cpp",
        },
        .flags = &.{ "-std=c++17", "-fno-exceptions", "-fno-rtti", "-fno-sanitize=alignment" },
    });

    // GDSTK Source Files
    const gdstk_src = &.{
        "vendor/gdstk/src/cell.cpp",
        "vendor/gdstk/src/clipper_tools.cpp",
        "vendor/gdstk/src/curve.cpp",
        "vendor/gdstk/src/flexpath.cpp",
        "vendor/gdstk/src/gdsii.cpp",
        "vendor/gdstk/src/label.cpp",
        "vendor/gdstk/src/library.cpp",
        "vendor/gdstk/src/oasis.cpp",
        "vendor/gdstk/src/polygon.cpp",
        "vendor/gdstk/src/property.cpp",
        "vendor/gdstk/src/rawcell.cpp",
        "vendor/gdstk/src/reference.cpp",
        "vendor/gdstk/src/repetition.cpp",
        "vendor/gdstk/src/robustpath.cpp",
        "vendor/gdstk/src/style.cpp",
        "vendor/gdstk/src/utils.cpp",
        "vendor/gdstk/external/clipper/clipper.cpp",
    };
    exe.addCSourceFiles(.{
        .files = gdstk_src,
        .flags = &.{ "-std=c++17", "-fno-rtti", "-DGDSTK_NO_PYTHON", "-DGDSTK_CUSTOM_ALLOCATOR", "-fno-sanitize=alignment" },
    });

    // Miniz Source Files
    exe.addCSourceFiles(.{ 
        .files = &.{ "vendor/miniz/miniz.c" },
    });

    exe.linkLibC();
    exe.linkLibCpp();

    if (target.result.cpu.arch.isWasm()) {
        exe.entry = .disabled;
        exe.root_module.strip = true;
        exe.stack_size = 1024 * 1024 * 4; // 4MB stack
        exe.root_module.addCMacro("WASM", "");
    }

    b.installArtifact(exe);

    const run_cmd = b.addRunArtifact(exe);
    run_cmd.step.dependOn(b.getInstallStep());
    if (b.args) |args| {
        run_cmd.addArgs(args);
    }
    const run_step = b.step("run", "Run the app");
    run_step.dependOn(&run_cmd.step);
}
