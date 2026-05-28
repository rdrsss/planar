//! Pure artifact helpers for downstream packaging/install code.
//!
//! These helpers return deterministic artifact names plus generated text. They
//! intentionally do not create directories, write files, gzip output, or depend
//! on `std.Build`; downstream packaging remains responsible for those choices.

const cmd_mod = @import("cmd.zig");
const completion_mod = @import("completion.zig");
const man_mod = @import("man.zig");
const schema_mod = @import("schema.zig");

pub const Artifact = struct {
    name: []const u8,
    data: []const u8,
};

pub fn manFileName(comptime root: cmd_mod.Cmd, comptime path: []const []const u8) []const u8 {
    return comptime man_mod.pageName(root, path) ++ ".1";
}

pub fn completionFileName(comptime root: cmd_mod.Cmd, comptime shell: completion_mod.Shell) []const u8 {
    return comptime switch (shell) {
        .bash => root.name ++ ".bash",
        .zsh => "_" ++ root.name,
        .fish => root.name ++ ".fish",
    };
}

pub fn schemaFileName(comptime root: cmd_mod.Cmd) []const u8 {
    return comptime root.name ++ ".schema.json";
}

pub fn manPage(
    comptime root: cmd_mod.Cmd,
    comptime path: []const []const u8,
    comptime options: man_mod.Options,
) Artifact {
    return comptime .{
        .name = manFileName(root, path),
        .data = man_mod.page(root, path, options),
    };
}

pub fn allManPages(comptime root: cmd_mod.Cmd, comptime options: man_mod.Options) []const Artifact {
    return comptime manArtifactsFromPages(man_mod.allPages(root, options));
}

pub fn completionScript(comptime root: cmd_mod.Cmd, comptime shell: completion_mod.Shell) Artifact {
    return comptime .{
        .name = completionFileName(root, shell),
        .data = completion_mod.script(root, shell),
    };
}

pub fn schemaJson(comptime root: cmd_mod.Cmd, comptime options: schema_mod.Options) Artifact {
    return comptime .{
        .name = schemaFileName(root),
        .data = schema_mod.json(root, options),
    };
}

fn manArtifactsFromPages(comptime pages: []const man_mod.Page) []const Artifact {
    comptime {
        var out: []const Artifact = &.{};
        for (pages) |p| {
            out = out ++ [_]Artifact{.{
                .name = p.name ++ ".1",
                .data = p.data,
            }};
        }
        return out;
    }
}
