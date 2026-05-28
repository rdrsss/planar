const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const cli_mod = b.addModule("cli", .{
        .root_source_file = b.path("src/cli/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const etc_cli_mod = b.addModule("etc_cli", .{
        .root_source_file = b.path("src/cli/root.zig"),
        .target = target,
        .optimize = optimize,
    });

    const basic_example = b.addExecutable(.{
        .name = "etc-cli-basic-example",
        .root_module = b.createModule(.{
            .root_source_file = b.path("examples/basic.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });

    const tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/cli/root.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });

    const run_tests = b.addRunArtifact(tests);

    const package_import_cli_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/package_import_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_package_import_cli_tests = b.addRunArtifact(package_import_cli_tests);

    const package_import_etc_cli_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/package_import_etc_cli_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "etc_cli", .module = etc_cli_mod },
            },
        }),
    });
    const run_package_import_etc_cli_tests = b.addRunArtifact(package_import_etc_cli_tests);

    const parser_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/parser_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_parser_contract_tests = b.addRunArtifact(parser_contract_tests);

    const parser_edge_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/parser_edge_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_parser_edge_tests = b.addRunArtifact(parser_edge_tests);

    const dispatch_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/dispatch_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_dispatch_contract_tests = b.addRunArtifact(dispatch_contract_tests);

    const help_completion_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/help_completion_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_help_completion_contract_tests = b.addRunArtifact(help_completion_contract_tests);

    const man_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/man_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_man_contract_tests = b.addRunArtifact(man_contract_tests);

    const schema_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/schema_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_schema_contract_tests = b.addRunArtifact(schema_contract_tests);

    const env_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/env_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_env_contract_tests = b.addRunArtifact(env_contract_tests);

    const artifacts_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/artifacts_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_artifacts_contract_tests = b.addRunArtifact(artifacts_contract_tests);

    const app_runner_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/app_runner_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_app_runner_contract_tests = b.addRunArtifact(app_runner_contract_tests);

    const alias_visibility_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/alias_visibility_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_alias_visibility_contract_tests = b.addRunArtifact(alias_visibility_contract_tests);

    const completion_value_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/completion_value_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_completion_value_contract_tests = b.addRunArtifact(completion_value_contract_tests);

    const parser_expansion_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/parser_expansion_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_parser_expansion_contract_tests = b.addRunArtifact(parser_expansion_contract_tests);

    const snapshot_contract_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/snapshot_contract_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_snapshot_contract_tests = b.addRunArtifact(snapshot_contract_tests);

    const parser_property_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("integration_tests/parser_property_test.zig"),
            .target = target,
            .optimize = optimize,
            .imports = &.{
                .{ .name = "cli", .module = cli_mod },
            },
        }),
    });
    const run_parser_property_tests = b.addRunArtifact(parser_property_tests);

    const run_compile_fail_tests = b.addSystemCommand(&.{ "sh", "scripts/compile_fail.sh" });
    const run_mandoc_lint = b.addSystemCommand(&.{ "sh", "scripts/mandoc_lint.sh" });
    const run_completion_lint = b.addSystemCommand(&.{ "sh", "scripts/completion_lint.sh" });

    const test_step = b.step("test", "Run unit and integration tests");
    test_step.dependOn(&run_tests.step);
    test_step.dependOn(&basic_example.step);
    test_step.dependOn(&run_package_import_cli_tests.step);
    test_step.dependOn(&run_package_import_etc_cli_tests.step);
    test_step.dependOn(&run_parser_contract_tests.step);
    test_step.dependOn(&run_parser_edge_tests.step);
    test_step.dependOn(&run_dispatch_contract_tests.step);
    test_step.dependOn(&run_help_completion_contract_tests.step);
    test_step.dependOn(&run_man_contract_tests.step);
    test_step.dependOn(&run_schema_contract_tests.step);
    test_step.dependOn(&run_env_contract_tests.step);
    test_step.dependOn(&run_artifacts_contract_tests.step);
    test_step.dependOn(&run_app_runner_contract_tests.step);
    test_step.dependOn(&run_alias_visibility_contract_tests.step);
    test_step.dependOn(&run_completion_value_contract_tests.step);
    test_step.dependOn(&run_parser_expansion_contract_tests.step);
    test_step.dependOn(&run_snapshot_contract_tests.step);
    test_step.dependOn(&run_parser_property_tests.step);
    test_step.dependOn(&run_compile_fail_tests.step);
    test_step.dependOn(&run_mandoc_lint.step);
    test_step.dependOn(&run_completion_lint.step);
}
