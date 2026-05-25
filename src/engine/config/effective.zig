//! engine/config/effective.zig — defaults + file + env → effective Config.
//!
//! Mirrors Go's src/internal/config/resolve.go (four-layer precedence):
//!   1. Env var (highest priority)
//!   2. Per-association override (when assoc_slug is non-null)
//!   3. Config file values
//!   4. Embedded defaults (lowest priority)
//!
//! The output is two things:
//!   - A `Config` struct (structured, convenient accessor)
//!   - An `EffectiveMap` (flat dotted-key → ValueWithSource, for --effective display)
//!
//! D-engine-pattern: pure functions, no DB, no goroutines, no interfaces.
//! D-env-overrides: env vars are passed in as std.process.Environ (test-injectable).
//! D-effective-shape: per-key provenance mirroring Go's Source enum.

const std = @import("std");
const parse = @import("parse.zig");

// =========================================================================
// Public types
// =========================================================================

/// Which configuration layer supplied a resolved value. Mirrors Go's Source.
pub const Provenance = enum {
    embedded_default,
    config_file,
    assoc_override,
    env,

    pub fn label(self: Provenance) []const u8 {
        return switch (self) {
            .embedded_default => "embedded default",
            .config_file => "config file",
            .assoc_override => "per-association override",
            .env => "env",
        };
    }
};

/// A resolved value together with its provenance. Env-sourced values also
/// carry the env var name for display purposes (mirrors Go's EnvVarName).
pub const ValueWithSource = struct {
    value: []const u8,
    source: Provenance,
    /// Non-empty when source == .env; holds the environment variable name.
    env_var_name: []const u8,
};

/// The effective map: flat dotted-key → ValueWithSource.
/// All strings are owned by the allocator passed to resolve().
pub const EffectiveMap = std.StringHashMapUnmanaged(ValueWithSource);

// =========================================================================
// Structured Config (mirrors Go's Config struct field-for-field)
// =========================================================================

pub const Config = struct {
    defaults: Defaults,
    workbench: Workbench,
    templates: Templates,
    external: External,
};

pub const Defaults = struct {
    vendor: []const u8,
    scope: []const u8,
};

pub const Workbench = struct {
    root: []const u8,
};

pub const Templates = struct {
    dir: []const u8,
    default_set: []const u8,
};

pub const External = struct {
    jira: ExternalJira,
    github_issues: ExternalGitHubIssues,
    github_projects: ExternalGitHubProjects,
};

pub const ExternalJira = struct {
    base_url: []const u8,
    user_env: []const u8,
    token_env: []const u8,
    status: JiraStatus,
};

pub const JiraStatus = struct {
    todo: []const u8,
    doing: []const u8,
    blocked: []const u8,
    done: []const u8,
};

pub const ExternalGitHubIssues = struct {
    auth: []const u8,
    token_env: []const u8,
    status: GitHubIssueStatus,
};

pub const GitHubIssueStatus = struct {
    todo: []const u8,
    doing: []const u8,
    done: []const u8,
};

pub const ExternalGitHubProjects = struct {
    parent_field_names: [][]const u8,
};

// =========================================================================
// Result
// =========================================================================

/// Returned from resolve(). All string fields inside config and effective
/// are owned by the allocator.
pub const Resolved = struct {
    config: Config,
    effective: EffectiveMap,

    /// Release all memory owned by this result.
    pub fn deinit(self: *Resolved, allocator: std.mem.Allocator) void {
        deinitConfig(&self.config, allocator);
        deinitEffectiveMap(&self.effective, allocator);
    }
};

pub const Error = error{ ParseFailed, OutOfMemory };

// =========================================================================
// Embedded defaults
// =========================================================================

const defaults_toml_content = @embedFile("defaults.toml");

// =========================================================================
// Public API
// =========================================================================

/// Resolve the effective configuration from the four layers.
///
/// file_content: the raw TOML bytes from the user's config.toml, or null
///   if the file does not exist.
/// environ: environment variable accessor (injectable for tests).
/// assoc_slug: per-association override slug, or null for no override.
///
/// Returns a Resolved value owned by `allocator`. Caller must call
/// result.deinit(allocator) when done.
pub fn resolve(
    allocator: std.mem.Allocator,
    file_content: ?[]const u8,
    environ: std.process.Environ,
    assoc_slug: ?[]const u8,
) Error!Resolved {
    // Parse the embedded defaults (always present).
    var def_pe: parse.ParseError = undefined;
    var def_map = parse.parse(allocator, defaults_toml_content, &def_pe) catch |e| switch (e) {
        error.ParseFailed => return error.ParseFailed,
        error.UnsupportedFeature => return error.ParseFailed,
        error.OutOfMemory => return error.OutOfMemory,
    };
    defer parse.deinitMap(&def_map, allocator);

    // Parse user's config file (null → empty map).
    var file_map: std.StringHashMapUnmanaged(parse.Value) = .{};
    if (file_content) |fc| {
        var file_pe: parse.ParseError = undefined;
        file_map = parse.parse(allocator, fc, &file_pe) catch |e| switch (e) {
            error.ParseFailed => return error.ParseFailed,
            error.UnsupportedFeature => return error.ParseFailed,
            error.OutOfMemory => return error.OutOfMemory,
        };
    }
    defer parse.deinitMap(&file_map, allocator);

    // Build the effective map.
    var eff: EffectiveMap = .{};
    errdefer deinitEffectiveMap(&eff, allocator);

    // Helper: pick a scalar string value from the four layers.
    // Returns an owned copy of the winning value + records provenance in eff.
    const PickArgs = struct {
        key: []const u8,
        env_name: ?[]const u8,
        assoc_val: ?[]const u8,
        file_key: ?[]const u8,
        def_key: ?[]const u8,
    };

    // pickStr: resolve a single string-typed key across the four layers.
    const pickStr = struct {
        fn call(
            alloc: std.mem.Allocator,
            env: std.process.Environ,
            fmap: *const std.StringHashMapUnmanaged(parse.Value),
            dmap: *const std.StringHashMapUnmanaged(parse.Value),
            effective: *EffectiveMap,
            args: PickArgs,
        ) Error![]const u8 {
            // Layer 1: env var.
            if (args.env_name) |en| {
                if (env.getPosix(en)) |ev| {
                    if (ev.len > 0) {
                        const v = try alloc.dupe(u8, ev);
                        const k = try alloc.dupe(u8, args.key);
                        const env_var = try alloc.dupe(u8, en);
                        const res = try effective.getOrPut(alloc, k);
                        if (res.found_existing) {
                            alloc.free(res.key_ptr.*);
                            alloc.free(res.value_ptr.value);
                            alloc.free(res.value_ptr.env_var_name);
                        }
                        res.key_ptr.* = k;
                        res.value_ptr.* = .{ .value = v, .source = .env, .env_var_name = env_var };
                        return v;
                    }
                }
            }

            // Layer 2: per-association override (if any).
            if (args.assoc_val) |av| {
                if (av.len > 0) {
                    const v = try alloc.dupe(u8, av);
                    const k = try alloc.dupe(u8, args.key);
                    const res = try effective.getOrPut(alloc, k);
                    if (res.found_existing) {
                        alloc.free(res.key_ptr.*);
                        alloc.free(res.value_ptr.value);
                        alloc.free(res.value_ptr.env_var_name);
                    }
                    res.key_ptr.* = k;
                    res.value_ptr.* = .{ .value = v, .source = .assoc_override, .env_var_name = "" };
                    return v;
                }
            }

            // Layer 3: config file.
            const file_lookup_key = args.file_key orelse args.key;
            if (fmap.get(file_lookup_key)) |fv| {
                const raw: []const u8 = switch (fv) {
                    .string => |s| s,
                    .bool => |b| if (b) "true" else "false",
                    .int => "", // int not expected for string fields
                    .array => "", // array not expected for string fields
                };
                if (raw.len > 0) {
                    const v = try alloc.dupe(u8, raw);
                    const k = try alloc.dupe(u8, args.key);
                    const res = try effective.getOrPut(alloc, k);
                    if (res.found_existing) {
                        alloc.free(res.key_ptr.*);
                        alloc.free(res.value_ptr.value);
                        alloc.free(res.value_ptr.env_var_name);
                    }
                    res.key_ptr.* = k;
                    res.value_ptr.* = .{ .value = v, .source = .config_file, .env_var_name = "" };
                    return v;
                }
            }

            // Layer 4: embedded defaults.
            const def_lookup_key = args.def_key orelse args.key;
            if (dmap.get(def_lookup_key)) |dv| {
                const raw: []const u8 = switch (dv) {
                    .string => |s| s,
                    .bool => |b| if (b) "true" else "false",
                    .int => "",
                    .array => "",
                };
                if (raw.len > 0) {
                    const v = try alloc.dupe(u8, raw);
                    const k = try alloc.dupe(u8, args.key);
                    const res = try effective.getOrPut(alloc, k);
                    if (res.found_existing) {
                        alloc.free(res.key_ptr.*);
                        alloc.free(res.value_ptr.value);
                        alloc.free(res.value_ptr.env_var_name);
                    }
                    res.key_ptr.* = k;
                    res.value_ptr.* = .{ .value = v, .source = .embedded_default, .env_var_name = "" };
                    return v;
                }
            }

            // Nothing found: return empty string (no provenance entry).
            return try alloc.dupe(u8, "");
        }
    }.call;

    // Assoc override lookup helper — returns the value of a key if it's in
    // the associations.<slug>.<subkey> section of the file map.
    // Since we store keys as dotted paths, we can directly look up
    // "associations.<slug>.<subkey>".
    const assocStr = struct {
        fn call(
            alloc: std.mem.Allocator,
            fmap: *const std.StringHashMapUnmanaged(parse.Value),
            slug: ?[]const u8,
            subkey: []const u8,
        ) Error!?[]const u8 {
            const s = slug orelse return null;
            // Build lookup key: associations.<slug>.<subkey>
            const lookup_key = try std.fmt.allocPrint(alloc, "associations.\"{s}\".{s}", .{ s, subkey });
            defer alloc.free(lookup_key);
            if (fmap.get(lookup_key)) |fv| {
                return switch (fv) {
                    .string => |sv| sv,
                    else => null,
                };
            }
            // Also try without quotes (bare slug).
            const bare_key = try std.fmt.allocPrint(alloc, "associations.{s}.{s}", .{ s, subkey });
            defer alloc.free(bare_key);
            if (fmap.get(bare_key)) |fv| {
                return switch (fv) {
                    .string => |sv| sv,
                    else => null,
                };
            }
            return null;
        }
    }.call;

    // Resolve each key following Go's resolve.go ordering exactly.
    const defaults_vendor = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "defaults.vendor",
        .env_name = "PLANAR_VENDOR",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });
    const defaults_scope = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "defaults.scope",
        .env_name = "PLANAR_SCOPE",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });
    const workbench_root = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "workbench.root",
        .env_name = "PLANAR_WORKBENCH_ROOT",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });
    const templates_dir = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "templates.dir",
        .env_name = "PLANAR_TEMPLATES_DIR",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });
    const templates_default_set = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "templates.default_set",
        .env_name = "PLANAR_TEMPLATES_DEFAULT_SET",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });

    // external.jira.base_url — env var is JIRA_BASE_URL (Go resolve.go L262).
    const jira_base_url = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.base_url",
        .env_name = "JIRA_BASE_URL",
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });

    // external.jira.user_env / token_env — no env override for these (they name env vars).
    const jira_user_env = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.user_env",
        .env_name = null,
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });
    const jira_token_env = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.token_env",
        .env_name = null,
        .assoc_val = null,
        .file_key = null,
        .def_key = null,
    });

    // Jira status — assoc overrides possible.
    const assoc_jira_todo = try assocStr(allocator, &file_map, assoc_slug, "external.jira.status.todo");
    const assoc_jira_doing = try assocStr(allocator, &file_map, assoc_slug, "external.jira.status.doing");
    const assoc_jira_blocked = try assocStr(allocator, &file_map, assoc_slug, "external.jira.status.blocked");
    const assoc_jira_done = try assocStr(allocator, &file_map, assoc_slug, "external.jira.status.done");

    const jira_status_todo = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.status.todo",
        .env_name = null,
        .assoc_val = assoc_jira_todo,
        .file_key = null,
        .def_key = null,
    });
    const jira_status_doing = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.status.doing",
        .env_name = null,
        .assoc_val = assoc_jira_doing,
        .file_key = null,
        .def_key = null,
    });
    const jira_status_blocked = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.status.blocked",
        .env_name = null,
        .assoc_val = assoc_jira_blocked,
        .file_key = null,
        .def_key = null,
    });
    const jira_status_done = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.jira.status.done",
        .env_name = null,
        .assoc_val = assoc_jira_done,
        .file_key = null,
        .def_key = null,
    });

    // external.github-issues.auth — env var PLANAR_GITHUB_AUTH.
    const gh_issues_auth = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.github-issues.auth",
        .env_name = "PLANAR_GITHUB_AUTH",
        .assoc_val = null,
        .file_key = "external.github-issues.auth",
        .def_key = "external.github-issues.auth",
    });
    const gh_issues_token_env = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.github-issues.token_env",
        .env_name = null,
        .assoc_val = null,
        .file_key = "external.github-issues.token_env",
        .def_key = "external.github-issues.token_env",
    });

    const gh_issues_status_todo = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.github-issues.status.todo",
        .env_name = null,
        .assoc_val = null,
        .file_key = "external.github-issues.status.todo",
        .def_key = "external.github-issues.status.todo",
    });
    const gh_issues_status_doing = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.github-issues.status.doing",
        .env_name = null,
        .assoc_val = null,
        .file_key = "external.github-issues.status.doing",
        .def_key = "external.github-issues.status.doing",
    });
    const gh_issues_status_done = try pickStr(allocator, environ, &file_map, &def_map, &eff, .{
        .key = "external.github-issues.status.done",
        .env_name = null,
        .assoc_val = null,
        .file_key = "external.github-issues.status.done",
        .def_key = "external.github-issues.status.done",
    });

    // external.github-projects.parent_field_names — slice, handled specially.
    var gh_projects_parent_field_names: [][]const u8 = &.{};
    gh_projects_parent_field_names = try resolveParentFieldNames(
        allocator,
        environ,
        &file_map,
        &def_map,
        &eff,
    );
    // Always free the intermediate slice — Config copies each string via dupe below.
    // Guard len>0: resolveParentFieldNames may return a static &.{} when no value found.
    defer if (gh_projects_parent_field_names.len > 0) {
        for (gh_projects_parent_field_names) |s| allocator.free(s);
        allocator.free(gh_projects_parent_field_names);
    };

    // Build Config with separate dupe allocations from EffectiveMap values.
    // Each pickStr result is owned by EffectiveMap; Config needs its own copy.
    const config = Config{
        .defaults = .{
            .vendor = try allocator.dupe(u8, defaults_vendor),
            .scope = try allocator.dupe(u8, defaults_scope),
        },
        .workbench = .{ .root = try allocator.dupe(u8, workbench_root) },
        .templates = .{
            .dir = try allocator.dupe(u8, templates_dir),
            .default_set = try allocator.dupe(u8, templates_default_set),
        },
        .external = .{
            .jira = .{
                .base_url = try allocator.dupe(u8, jira_base_url),
                .user_env = try allocator.dupe(u8, jira_user_env),
                .token_env = try allocator.dupe(u8, jira_token_env),
                .status = .{
                    .todo = try allocator.dupe(u8, jira_status_todo),
                    .doing = try allocator.dupe(u8, jira_status_doing),
                    .blocked = try allocator.dupe(u8, jira_status_blocked),
                    .done = try allocator.dupe(u8, jira_status_done),
                },
            },
            .github_issues = .{
                .auth = try allocator.dupe(u8, gh_issues_auth),
                .token_env = try allocator.dupe(u8, gh_issues_token_env),
                .status = .{
                    .todo = try allocator.dupe(u8, gh_issues_status_todo),
                    .doing = try allocator.dupe(u8, gh_issues_status_doing),
                    .done = try allocator.dupe(u8, gh_issues_status_done),
                },
            },
            .github_projects = .{
                // parent_field_names: Config gets its own copies.
                .parent_field_names = blk: {
                    var cfg_names = try allocator.alloc([]const u8, gh_projects_parent_field_names.len);
                    for (gh_projects_parent_field_names, 0..) |s, i| {
                        cfg_names[i] = try allocator.dupe(u8, s);
                    }
                    break :blk cfg_names;
                },
            },
        },
    };

    return Resolved{ .config = config, .effective = eff };
}

/// Handle the special-case parent_field_names (array type). Mirrors Go's
/// resolveParentFieldNames in resolve.go.
fn resolveParentFieldNames(
    allocator: std.mem.Allocator,
    environ: std.process.Environ,
    file_map: *const std.StringHashMapUnmanaged(parse.Value),
    def_map: *const std.StringHashMapUnmanaged(parse.Value),
    eff: *EffectiveMap,
) Error![][]const u8 {
    const key = "external.github-projects.parent_field_names";

    // Layer 1: env var PLANAR_GITHUB_PROJECTS_PARENT_FIELDS (comma-separated).
    if (environ.getPosix("PLANAR_GITHUB_PROJECTS_PARENT_FIELDS")) |ev| {
        if (ev.len > 0) {
            const fields = try splitAndTrim(allocator, ev, ',');
            const joined = try std.mem.join(allocator, ", ", fields);
            defer allocator.free(joined);
            const k = try allocator.dupe(u8, key);
            const v = try allocator.dupe(u8, joined);
            const env_var = try allocator.dupe(u8, "PLANAR_GITHUB_PROJECTS_PARENT_FIELDS");
            const res = try eff.getOrPut(allocator, k);
            if (res.found_existing) {
                allocator.free(res.key_ptr.*);
                allocator.free(res.value_ptr.value);
                allocator.free(res.value_ptr.env_var_name);
            }
            res.key_ptr.* = k;
            res.value_ptr.* = .{ .value = v, .source = .env, .env_var_name = env_var };
            return fields;
        }
    }

    // Layer 3: config file.
    if (file_map.get(key)) |fv| {
        if (fv == .array and fv.array.len > 0) {
            var owned_list: std.ArrayList([]const u8) = .empty;
            errdefer {
                for (owned_list.items) |s| allocator.free(s);
                owned_list.deinit(allocator);
            }
            for (fv.array) |s| {
                try owned_list.append(allocator, try allocator.dupe(u8, s));
            }
            const owned = try owned_list.toOwnedSlice(allocator);
            errdefer {
                for (owned) |s| allocator.free(s);
                allocator.free(owned);
            }
            const joined = try std.mem.join(allocator, ", ", owned);
            defer allocator.free(joined);
            const k = try allocator.dupe(u8, key);
            const v = try allocator.dupe(u8, joined);
            const res = try eff.getOrPut(allocator, k);
            if (res.found_existing) {
                allocator.free(res.key_ptr.*);
                allocator.free(res.value_ptr.value);
                allocator.free(res.value_ptr.env_var_name);
            }
            res.key_ptr.* = k;
            res.value_ptr.* = .{ .value = v, .source = .config_file, .env_var_name = "" };
            return owned;
        }
    }

    // Layer 4: embedded defaults.
    if (def_map.get(key)) |dv| {
        if (dv == .array and dv.array.len > 0) {
            var owned_list: std.ArrayList([]const u8) = .empty;
            errdefer {
                for (owned_list.items) |s| allocator.free(s);
                owned_list.deinit(allocator);
            }
            for (dv.array) |s| {
                try owned_list.append(allocator, try allocator.dupe(u8, s));
            }
            const owned = try owned_list.toOwnedSlice(allocator);
            errdefer {
                for (owned) |s| allocator.free(s);
                allocator.free(owned);
            }
            const joined = try std.mem.join(allocator, ", ", owned);
            defer allocator.free(joined);
            const k = try allocator.dupe(u8, key);
            const v = try allocator.dupe(u8, joined);
            const res = try eff.getOrPut(allocator, k);
            if (res.found_existing) {
                allocator.free(res.key_ptr.*);
                allocator.free(res.value_ptr.value);
                allocator.free(res.value_ptr.env_var_name);
            }
            res.key_ptr.* = k;
            res.value_ptr.* = .{ .value = v, .source = .embedded_default, .env_var_name = "" };
            return owned;
        }
    }

    return &.{};
}

/// Split a string by delimiter and trim whitespace from each part.
/// Returns an owned slice of owned strings.
fn splitAndTrim(allocator: std.mem.Allocator, s: []const u8, delim: u8) Error![][]const u8 {
    var parts: std.ArrayList([]const u8) = .empty;
    errdefer {
        for (parts.items) |p| allocator.free(p);
        parts.deinit(allocator);
    }
    var it = std.mem.splitScalar(u8, s, delim);
    while (it.next()) |part| {
        const trimmed = std.mem.trim(u8, part, " \t");
        if (trimmed.len > 0) {
            try parts.append(allocator, try allocator.dupe(u8, trimmed));
        }
    }
    return parts.toOwnedSlice(allocator);
}

// =========================================================================
// Memory cleanup helpers
// =========================================================================

pub fn deinitConfig(cfg: *Config, allocator: std.mem.Allocator) void {
    allocator.free(cfg.defaults.vendor);
    allocator.free(cfg.defaults.scope);
    allocator.free(cfg.workbench.root);
    allocator.free(cfg.templates.dir);
    allocator.free(cfg.templates.default_set);
    allocator.free(cfg.external.jira.base_url);
    allocator.free(cfg.external.jira.user_env);
    allocator.free(cfg.external.jira.token_env);
    allocator.free(cfg.external.jira.status.todo);
    allocator.free(cfg.external.jira.status.doing);
    allocator.free(cfg.external.jira.status.blocked);
    allocator.free(cfg.external.jira.status.done);
    allocator.free(cfg.external.github_issues.auth);
    allocator.free(cfg.external.github_issues.token_env);
    allocator.free(cfg.external.github_issues.status.todo);
    allocator.free(cfg.external.github_issues.status.doing);
    allocator.free(cfg.external.github_issues.status.done);
    for (cfg.external.github_projects.parent_field_names) |s| allocator.free(s);
    allocator.free(cfg.external.github_projects.parent_field_names);
}

pub fn deinitEffectiveMap(eff: *EffectiveMap, allocator: std.mem.Allocator) void {
    var it = eff.iterator();
    while (it.next()) |entry| {
        allocator.free(entry.key_ptr.*);
        allocator.free(entry.value_ptr.value);
        allocator.free(entry.value_ptr.env_var_name);
    }
    eff.deinit(allocator);
}

// =========================================================================
// Sensitive field masking (mirrors Go's sensitive.go)
// =========================================================================

/// Report whether a key name refers to a sensitive value that should be
/// masked in `config show` output (without --raw). Mirrors Go's SensitiveName.
pub fn sensitiveName(name: []const u8) bool {
    const suffixes = [_][]const u8{ "_token", "_password", "_secret", "_key" };
    const exact = [_][]const u8{ "token", "password", "secret" };

    // Mirror Go's SensitiveName: case-insensitive match via ToLower.
    // Stack-buffer up to 256 chars (any realistic config key fits); for longer
    // keys fall back to byte-compare on the original (no realistic config has
    // sensitive names exceeding this length).
    var lower_buf: [256]u8 = undefined;
    const lower = if (name.len <= lower_buf.len)
        std.ascii.lowerString(lower_buf[0..name.len], name)
    else
        name;

    for (exact) |e| {
        if (std.mem.eql(u8, lower, e)) return true;
    }
    for (suffixes) |suf| {
        if (std.mem.endsWith(u8, lower, suf)) return true;
    }
    return false;
}

// =========================================================================
// Sorted keys helper (mirrors Go's Resolved.Keys())
// =========================================================================

/// Return a sorted slice of all effective map keys. Caller frees the slice
/// (but not the key strings — those are owned by the effective map).
pub fn sortedKeys(eff: *const EffectiveMap, allocator: std.mem.Allocator) Error![][]const u8 {
    var keys = try allocator.alloc([]const u8, eff.count());
    var i: usize = 0;
    var it = eff.iterator();
    while (it.next()) |entry| {
        keys[i] = entry.key_ptr.*;
        i += 1;
    }
    std.mem.sort([]const u8, keys, {}, struct {
        fn lt(_: void, a: []const u8, b: []const u8) bool {
            return std.mem.lessThan(u8, a, b);
        }
    }.lt);
    return keys;
}

// =========================================================================
// Test helpers
// =========================================================================

/// Build an Environ from a flat list of "KEY=value" strings.
/// The caller must call freeTestEnviron(allocator, result.block) when done.
fn testEnvironFrom(allocator: std.mem.Allocator, entries: []const []const u8) !struct {
    block: std.process.Environ.PosixBlock,
    environ: std.process.Environ,
    // Keep owned copies for deallocation.
    owned: [][:0]const u8,
    envp: [:null]?[*:0]u8,
} {
    const envp = try allocator.allocSentinel(?[*:0]u8, entries.len, null);
    errdefer allocator.free(envp);

    const owned = try allocator.alloc([:0]const u8, entries.len);
    errdefer allocator.free(owned);

    for (entries, 0..) |entry, i| {
        const z = try allocator.dupeZ(u8, entry);
        owned[i] = z;
        // We need a mutable pointer but dupeZ returns const — safe to cast
        // since we own the allocation and know it's writable.
        envp[i] = @constCast(z.ptr);
    }

    const block = std.process.Environ.PosixBlock{ .slice = envp };
    const environ = std.process.Environ{ .block = block };
    return .{ .block = block, .environ = environ, .owned = owned, .envp = envp };
}

fn freeTestEnviron(allocator: std.mem.Allocator, owned: [][:0]const u8, envp: [:null]?[*:0]u8) void {
    for (owned) |s| allocator.free(s);
    allocator.free(owned);
    allocator.free(envp);
}

// =========================================================================
// Tests
// =========================================================================

test "effective: defaults-only (no file, no env) — all keys come from embedded default" {
    const a = std.testing.allocator;
    const environ = std.process.Environ.empty;

    var res = try resolve(a, null, environ, null);
    defer res.deinit(a);

    try std.testing.expectEqualStrings("claude", res.config.defaults.vendor);
    try std.testing.expectEqualStrings("global", res.config.defaults.scope);
    try std.testing.expectEqualStrings("~/.planar/workbench", res.config.workbench.root);
    try std.testing.expectEqualStrings("gh-cli", res.config.external.github_issues.auth);
    try std.testing.expectEqualStrings("To Do", res.config.external.jira.status.todo);

    // All provenance should be embedded_default.
    const vendor_prov = res.effective.get("defaults.vendor") orelse
        return error.TestFailed;
    try std.testing.expectEqual(Provenance.embedded_default, vendor_prov.source);
    try std.testing.expectEqualStrings("", vendor_prov.env_var_name);
}

test "effective: file overrides default vendor" {
    const a = std.testing.allocator;
    const environ = std.process.Environ.empty;

    const file_content =
        \\[defaults]
        \\vendor = "codex"
    ;

    var res = try resolve(a, file_content, environ, null);
    defer res.deinit(a);

    try std.testing.expectEqualStrings("codex", res.config.defaults.vendor);
    const vendor_prov = res.effective.get("defaults.vendor") orelse
        return error.TestFailed;
    try std.testing.expectEqual(Provenance.config_file, vendor_prov.source);
}

test "effective: env override beats file" {
    const a = std.testing.allocator;
    const te = try testEnvironFrom(a, &.{"PLANAR_VENDOR=claude"});
    defer freeTestEnviron(a, te.owned, te.envp);

    const file_content =
        \\[defaults]
        \\vendor = "codex"
    ;

    var res = try resolve(a, file_content, te.environ, null);
    defer res.deinit(a);

    // env wins.
    try std.testing.expectEqualStrings("claude", res.config.defaults.vendor);
    const vendor_prov = res.effective.get("defaults.vendor") orelse
        return error.TestFailed;
    try std.testing.expectEqual(Provenance.env, vendor_prov.source);
    try std.testing.expectEqualStrings("PLANAR_VENDOR", vendor_prov.env_var_name);
}

test "effective: parent_field_names env override (comma-separated)" {
    const a = std.testing.allocator;
    const te = try testEnvironFrom(a, &.{"PLANAR_GITHUB_PROJECTS_PARENT_FIELDS=Epic, Feature"});
    defer freeTestEnviron(a, te.owned, te.envp);

    var res = try resolve(a, null, te.environ, null);
    defer res.deinit(a);

    const names = res.config.external.github_projects.parent_field_names;
    try std.testing.expectEqual(@as(usize, 2), names.len);
    try std.testing.expectEqualStrings("Epic", names[0]);
    try std.testing.expectEqualStrings("Feature", names[1]);

    const prov = res.effective.get("external.github-projects.parent_field_names") orelse
        return error.TestFailed;
    try std.testing.expectEqual(Provenance.env, prov.source);
}

test "sensitiveName: token suffix is sensitive" {
    try std.testing.expect(sensitiveName("token_env") == false);
    try std.testing.expect(sensitiveName("api_token") == true);
    try std.testing.expect(sensitiveName("jira_password") == true);
    try std.testing.expect(sensitiveName("vendor") == false);
    try std.testing.expect(sensitiveName("token") == true);
}

test "sensitiveName: case-insensitive (mirrors Go's strings.ToLower)" {
    try std.testing.expect(sensitiveName("API_TOKEN") == true);
    try std.testing.expect(sensitiveName("Api_Token") == true);
    try std.testing.expect(sensitiveName("JIRA_PASSWORD") == true);
    try std.testing.expect(sensitiveName("TOKEN") == true);
    try std.testing.expect(sensitiveName("PASSWORD") == true);
    try std.testing.expect(sensitiveName("SECRET") == true);
    try std.testing.expect(sensitiveName("Vendor") == false);
}

test "sortedKeys: keys come back in lexicographic order" {
    const a = std.testing.allocator;
    const environ = std.process.Environ.empty;
    var res = try resolve(a, null, environ, null);
    defer res.deinit(a);

    const keys = try sortedKeys(&res.effective, a);
    defer a.free(keys);

    try std.testing.expect(keys.len > 0);
    // Verify sorted.
    for (keys[0 .. keys.len - 1], keys[1..]) |a_k, b_k| {
        try std.testing.expect(std.mem.lessThan(u8, a_k, b_k) or std.mem.eql(u8, a_k, b_k));
    }
}
