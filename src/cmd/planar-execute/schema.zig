//! schema.zig — CLI schema ingestion for `planar-execute`.
//!
//! Shells `<bin> schema` (binary name as a parameter; resolved via PATH) and
//! parses the flat JSON catalog into typed Zig structs. The resulting
//! `BinSchema` is the queryable surface the brief compiler (task 3171) will
//! consume to enumerate the exact verbs and flags available to the worker.
//!
//! ## Capability boundary
//!
//! This module holds NO SQLite handle and imports NO db/engine/runtime module.
//! Every schema read is a subprocess call (`std.process.run`) followed by
//! `std.json.parseFromSlice` with `ignore_unknown_fields = true`. The structs
//! declare only the fields the brief compiler needs.
//!
//! ## Primary worker surface
//!
//! The worker's constrained PATH is `planar-agent` (the claim ritual:
//! `pull`/`claim`/`heartbeat`/`complete`/`fail`/`release`/`block`) plus
//! `git`. Ingest `planar-agent` schema via `loadSchema("planar-agent", ...)`.
//! The helper is binary-parameterized so `planar` schema can also be ingested
//! when the brief compiler needs the operator surface.
//!
//! ## Real schema shape (captured from `./bin/planar-agent schema` 2026-06-03)
//!
//! Top-level keys: `schemaVersion` (int), `layout` (string), `root` (string),
//! `commands` (array). Each command object:
//!   { "name": "complete",
//!     "aliases": [],
//!     "hidden": false,
//!     "deprecated": null,
//!     "path": ["complete"],
//!     "command": "planar-agent complete",
//!     "summary": "...",
//!     "description": "...",
//!     "subcommands": [],
//!     "flags": [
//!       { "long": "--claim", "aliases": [], "hidden": false, "deprecated": null,
//!         "short": null, "kind": "string", "required": true, "source": "local",
//!         "valueName": "VALUE", "default": null, "description": "...",
//!         "completion": {"kind": "none", "values": []}, "env": null }
//!     ],
//!     "positionals": [...],
//!     "docs": { ... } }
//!
//! ## Memory ownership
//!
//! `loadSchema` returns a `std.json.Parsed(RawSchema)` wrapper. The caller
//! owns the memory and MUST call `.deinit()` when done.
//!
//! `BinSchema.init` wraps the parsed result and borrows from its arena — the
//! `BinSchema` is valid only while the `std.json.Parsed(RawSchema)` is alive.
//! Call `.commands()` on the `BinSchema` to iterate `CommandEntry` values.
//!
//! ## Stdout limit
//!
//! `planar-agent schema` emits ~30 KiB. We cap stdout at 512 KiB — generous
//! for growth but safe against pathological output.
//!
//! ## Error mapping
//!
//! All subprocess and parse errors are mapped into the `SchemaError` set.
//! No error is silently swallowed.

const std = @import("std");
const Io = std.Io;

// ---------------------------------------------------------------------------
// Error set
// ---------------------------------------------------------------------------

/// All errors this module can surface. Callers inspect these rather than
/// catching `anyerror`.
pub const SchemaError = error{
    /// `std.process.run` itself failed (could not spawn the child, broken
    /// pipe reading stdout, etc.). Treat as a transient / configuration error.
    SubprocessFailed,
    /// The child exited with a non-zero status code. The binary exits
    /// non-zero on bad flags, DB errors, etc.
    SubprocessNonZero,
    /// `std.json.parseFromSlice` rejected the output (not valid JSON, or a
    /// required field is missing / wrong type).
    ParseFailed,
    /// Allocator returned OOM.
    OutOfMemory,
};

// ---------------------------------------------------------------------------
// Typed structs — minimal; ignore_unknown_fields covers the rest.
// ---------------------------------------------------------------------------

/// A single flag on a command, from `<bin> schema`'s `flags` array.
///
/// Real shape (captured 2026-06-03):
///   { "long": "--claim", "aliases": [], "hidden": false, "deprecated": null,
///     "short": null, "kind": "string", "required": true, ... }
///
/// Fields declared: only those the brief compiler consumes.
pub const FlagEntry = struct {
    /// The canonical long flag name, e.g. `"--claim"`.
    long: []const u8,
    /// Alias long names (often empty).
    aliases: [][]const u8 = &.{},
    /// Optional single-char short flag, e.g. `'h'` for `-h`. Null when absent.
    short: ?u8 = null,
    /// Whether this flag is required.
    required: bool = false,
    /// Human-readable description of the flag's purpose.
    description: []const u8 = "",
};

/// A single command entry from `<bin> schema`'s `commands` array.
///
/// Real shape (captured 2026-06-03):
///   { "name": "complete", "command": "planar-agent complete",
///     "subcommands": [], "flags": [...], "positionals": [...], ... }
///
/// The `command` field is the full path string (e.g. `"planar-agent complete"`)
/// that uniquely identifies the command — the brief compiler uses this as the
/// display key. `name` is the leaf name only.
pub const CommandEntry = struct {
    /// Leaf command name, e.g. `"complete"`.
    name: []const u8,
    /// Full command path string, e.g. `"planar-agent complete"`.
    command: []const u8,
    /// Direct subcommand leaf names (not full paths).
    subcommands: [][]const u8 = &.{},
    /// Flags registered on this command (includes inherited globals).
    flags: []FlagEntry = &.{},
    /// Whether the command is marked hidden (internal/deprecated).
    hidden: bool = false,
};

/// The top-level `<bin> schema` JSON object.
///
/// Real top-level keys: `schemaVersion`, `layout`, `root`, `commands`.
/// We declare only the fields we consume; extras are silently ignored.
pub const RawSchema = struct {
    /// Schema format version (currently `1`).
    schemaVersion: u32,
    /// Layout kind (currently `"flat"` — all commands in one array).
    layout: []const u8,
    /// The binary root name, e.g. `"planar-agent"`.
    root: []const u8,
    /// Flat list of all commands in the tree (root + all leaves).
    commands: []CommandEntry,
};

// ---------------------------------------------------------------------------
// BinSchema — queryable surface over a parsed RawSchema.
// ---------------------------------------------------------------------------

/// A queryable wrapper around a parsed `RawSchema`.
///
/// ## Accessor contract (for the brief compiler — task 3171)
///
/// `BinSchema.commands()` returns the full flat slice of `CommandEntry`
/// values from which the brief compiler can build its enumeration:
///
///   const schema = try loadSchema(alloc, io, "planar-agent");
///   defer schema.deinit();
///   const bs = BinSchema.init(schema.value);
///   for (bs.commands()) |cmd| {
///       // cmd.command — full path string, e.g. "planar-agent complete"
///       // cmd.flags   — slice of FlagEntry
///       for (cmd.flags) |f| { /* f.long, f.aliases, f.short, f.required */ }
///   }
///
/// `BinSchema.findCommand(full_path)` returns a pointer to the `CommandEntry`
/// for the given full command path string, or null if not found. This is the
/// primary lookup the brief compiler uses for a specific verb:
///
///   if (bs.findCommand("planar-agent complete")) |cmd| {
///       // cmd.flags contains the flags for "planar-agent complete"
///   }
///
/// ## Memory lifetime
///
/// `BinSchema` borrows from the `std.json.Parsed(RawSchema)` arena. The
/// `Parsed` value MUST stay alive for the lifetime of the `BinSchema`.
pub const BinSchema = struct {
    raw: RawSchema,

    /// Wrap a `RawSchema` value (borrowed from the Parsed arena).
    pub fn init(raw: RawSchema) BinSchema {
        return .{ .raw = raw };
    }

    /// The binary root name (e.g. `"planar-agent"` or `"planar"`).
    pub fn root(self: BinSchema) []const u8 {
        return self.raw.root;
    }

    /// The full flat command list (root command + all subcommands).
    /// Each entry has `.command` (full path), `.flags`, `.subcommands`.
    pub fn commands(self: BinSchema) []CommandEntry {
        return self.raw.commands;
    }

    /// Look up a command by its full path string (e.g. `"planar-agent complete"`).
    /// Returns a pointer into the borrowed slice (valid while the Parsed arena is alive),
    /// or null if not found.
    pub fn findCommand(self: BinSchema, full_path: []const u8) ?*CommandEntry {
        for (self.raw.commands) |*cmd| {
            if (std.mem.eql(u8, cmd.command, full_path)) return cmd;
        }
        return null;
    }

    /// Return the schema format version.
    pub fn schemaVersion(self: BinSchema) u32 {
        return self.raw.schemaVersion;
    }
};

// ---------------------------------------------------------------------------
// Internal subprocess helper
// ---------------------------------------------------------------------------

/// spawnBin runs `<bin_name> schema` and returns the captured stdout.
///
/// `bin_name` is resolved via PATH (argv[0] = `bin_name`, NOT a hard-coded
/// `./bin/` prefix). The caller owns the returned slice and must free it.
/// Maps subprocess and non-zero-exit errors into `SchemaError`.
fn spawnBin(
    allocator: std.mem.Allocator,
    io: Io,
    bin_name: []const u8,
) SchemaError![]u8 {
    // Build argv: fixed 2-element stack array — always [bin_name, "schema"].
    // No ArrayList needed; the size is a compile-time constant. (task 3237)
    const argv = [_][]const u8{ bin_name, "schema" };

    // 512 KiB cap: generous for schema growth, safe against pathological output.
    const stdout_limit: usize = 512 * 1024;

    const result = std.process.run(allocator, io, .{
        .argv = &argv,
        .stdout_limit = Io.Limit.limited(stdout_limit),
        .stderr_limit = Io.Limit.limited(4096),
    }) catch return SchemaError.SubprocessFailed;

    // Free stderr immediately (not used by callers).
    allocator.free(result.stderr);

    // Map non-zero exit to an error. Free stdout before returning the error.
    const exit_ok = result.term == .exited and result.term.exited == 0;
    if (!exit_ok) {
        allocator.free(result.stdout);
        return SchemaError.SubprocessNonZero;
    }

    return result.stdout;
}

// ---------------------------------------------------------------------------
// Public helper
// ---------------------------------------------------------------------------

/// loadSchema shells `<bin_name> schema` and returns a
/// `std.json.Parsed(RawSchema)`.
///
/// `bin_name` is the binary name as resolved via PATH — pass `"planar-agent"`
/// for the worker surface or `"planar"` for the operator surface. Do NOT
/// pass a full path like `"./bin/planar-agent"` (the subprocess resolves via
/// PATH, mirroring `state.zig`'s `spawnPlanar` convention).
///
/// The caller owns the memory and MUST call `.deinit()` on the returned value.
///
/// ## Example usage (brief compiler)
///
///   const parsed = try schema.loadSchema(alloc, io, "planar-agent");
///   defer parsed.deinit();
///   const bs = schema.BinSchema.init(parsed.value);
///   for (bs.commands()) |cmd| { ... }
///   if (bs.findCommand("planar-agent complete")) |cmd| { ... }
pub fn loadSchema(
    allocator: std.mem.Allocator,
    io: Io,
    bin_name: []const u8,
) SchemaError!std.json.Parsed(RawSchema) {
    const stdout = try spawnBin(allocator, io, bin_name);
    defer allocator.free(stdout);

    return std.json.parseFromSlice(RawSchema, allocator, stdout, .{
        .ignore_unknown_fields = true,
    }) catch return SchemaError.ParseFailed;
}

// ---------------------------------------------------------------------------
// Unit tests — fixture-parse only, no live binary required.
// ---------------------------------------------------------------------------

// Representative trimmed fixture from `./bin/planar-agent schema` 2026-06-03.
// Keeps the real structure: schemaVersion=1, layout="flat", root="planar-agent",
// and a representative subset of commands (root, complete, heartbeat, pull, schema)
// with their real flags. Extra fields (docs, hidden, deprecated, path, summary,
// description, valueName, source, completion, env, kind, default) are present
// to exercise ignore_unknown_fields.
const fixture_agent_schema =
    \\{"schemaVersion":1,"layout":"flat","root":"planar-agent","commands":[
    \\  {"name":"planar-agent","aliases":[],"hidden":false,"deprecated":null,
    \\   "path":[],"command":"planar-agent",
    \\   "summary":"Agent-callable coordination binary (pull / claim / complete / heartbeat / reconcile).",
    \\   "description":"Agent-callable coordination binary.",
    \\   "subcommands":["version","pull","complete","fail","release","block","claim","heartbeat","action","schema"],
    \\   "flags":[],"positionals":[],
    \\   "docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],
    \\           "authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},
    \\  {"name":"complete","aliases":[],"hidden":false,"deprecated":null,
    \\   "path":["complete"],"command":"planar-agent complete",
    \\   "summary":"Atomically end the work session: task → done, claim → completed.",
    \\   "description":"Atomically end the work session: task → done, claim → completed.",
    \\   "subcommands":[],"flags":[
    \\     {"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":true,"source":"local","valueName":"VALUE",
    \\      "default":null,"description":"Claim token returned by pull/claim",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--summary","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":false,"source":"local","valueName":"VALUE",
    \\      "default":null,"description":"Free-text completion summary recorded on the action",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"bool","required":false,"source":"local","valueName":"",
    \\      "default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}
    \\   ],"positionals":[],
    \\   "docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],
    \\           "authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},
    \\  {"name":"heartbeat","aliases":[],"hidden":false,"deprecated":null,
    \\   "path":["heartbeat"],"command":"planar-agent heartbeat",
    \\   "summary":"Refresh the lease on an active claim.",
    \\   "description":"Refresh the lease on an active claim.",
    \\   "subcommands":[],"flags":[
    \\     {"long":"--claim","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":true,"source":"local","valueName":"VALUE",
    \\      "default":null,"description":"Claim token to refresh",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":false,"source":"local","valueName":"VALUE",
    \\      "default":"600","description":"New TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"bool","required":false,"source":"local","valueName":"",
    \\      "default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}
    \\   ],"positionals":[],
    \\   "docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],
    \\           "authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},
    \\  {"name":"pull","aliases":[],"hidden":false,"deprecated":null,
    \\   "path":["pull"],"command":"planar-agent pull",
    \\   "summary":"Atomically pick the next eligible task, claim it, and flip status to doing.",
    \\   "description":"Atomically pick the next eligible task, claim it, and flip status to doing.",
    \\   "subcommands":[],"flags":[
    \\     {"long":"--vendor","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":false,"source":"local","valueName":"VALUE",
    \\      "default":"planar-agent","description":"Vendor tag (default: planar-agent)",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--ttl","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"string","required":false,"source":"local","valueName":"VALUE",
    \\      "default":"600","description":"Lease TTL (default 600s; accepts bare int seconds or suffixed duration: 10m, 1h, 500ms)",
    \\      "completion":{"kind":"none","values":[]},"env":null},
    \\     {"long":"--json","aliases":[],"hidden":false,"deprecated":null,"short":null,
    \\      "kind":"bool","required":false,"source":"local","valueName":"",
    \\      "default":false,"description":"","completion":{"kind":"none","values":[]},"env":null}
    \\   ],"positionals":[
    \\     {"name":"plan-id","kind":"int","required":true,"description":"Plan id to pull from",
    \\      "completion":{"kind":"none","values":[]}}
    \\   ],
    \\   "docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],
    \\           "authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}},
    \\  {"name":"schema","aliases":[],"hidden":false,"deprecated":null,
    \\   "path":["schema"],"command":"planar-agent schema",
    \\   "summary":"Print the full command tree as a JSON catalog (flags, aliases, positionals).",
    \\   "description":"Print the full command tree as a JSON catalog (flags, aliases, positionals).",
    \\   "subcommands":[],"flags":[],"positionals":[],
    \\   "docs":{"examples":[],"exitCodes":[],"notes":[],"seeAlso":[],"files":[],"bugs":[],
    \\           "authors":[],"homepage":"","license":"","copyright":"","version":"","sourceUrl":""}}
    \\]}
;

test "RawSchema: parse real planar-agent fixture — top-level fields decoded" {
    // Top-level fields: schemaVersion, layout, root, commands.
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u32, 1), parsed.value.schemaVersion);
    try std.testing.expectEqualStrings("flat", parsed.value.layout);
    try std.testing.expectEqualStrings("planar-agent", parsed.value.root);
    // Fixture has 5 commands.
    try std.testing.expectEqual(@as(usize, 5), parsed.value.commands.len);
}

test "RawSchema: 'planar-agent complete' command — flags decoded (--claim + --summary + --json)" {
    // The key worker verb: complete. Flags: --claim (required), --summary, --json.
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    // Find 'planar-agent complete' in the flat list.
    var found: ?CommandEntry = null;
    for (parsed.value.commands) |cmd| {
        if (std.mem.eql(u8, cmd.command, "planar-agent complete")) {
            found = cmd;
            break;
        }
    }
    const cmd = found orelse return error.TestUnexpectedNull;

    try std.testing.expectEqualStrings("complete", cmd.name);
    try std.testing.expectEqualStrings("planar-agent complete", cmd.command);
    try std.testing.expectEqual(@as(usize, 3), cmd.flags.len);

    // --claim: required string flag.
    try std.testing.expectEqualStrings("--claim", cmd.flags[0].long);
    try std.testing.expectEqual(true, cmd.flags[0].required);
    try std.testing.expectEqualStrings("Claim token returned by pull/claim", cmd.flags[0].description);

    // --summary: optional string flag.
    try std.testing.expectEqualStrings("--summary", cmd.flags[1].long);
    try std.testing.expectEqual(false, cmd.flags[1].required);

    // --json: optional bool flag.
    try std.testing.expectEqualStrings("--json", cmd.flags[2].long);
    try std.testing.expectEqual(false, cmd.flags[2].required);
}

test "RawSchema: 'planar-agent heartbeat' command — --claim and --ttl flags" {
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    var found: ?CommandEntry = null;
    for (parsed.value.commands) |cmd| {
        if (std.mem.eql(u8, cmd.command, "planar-agent heartbeat")) {
            found = cmd;
            break;
        }
    }
    const cmd = found orelse return error.TestUnexpectedNull;

    try std.testing.expectEqual(@as(usize, 3), cmd.flags.len);
    try std.testing.expectEqualStrings("--claim", cmd.flags[0].long);
    try std.testing.expectEqual(true, cmd.flags[0].required);
    try std.testing.expectEqualStrings("--ttl", cmd.flags[1].long);
    try std.testing.expectEqual(false, cmd.flags[1].required);
}

test "RawSchema: root command — subcommands list includes worker verbs" {
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    // Root command is first in the flat list (command == "planar-agent", path == []).
    const root_cmd = parsed.value.commands[0];
    try std.testing.expectEqualStrings("planar-agent", root_cmd.command);
    try std.testing.expectEqual(@as(usize, 0), root_cmd.flags.len);

    // Subcommands slice lists worker verbs by leaf name.
    const subs = root_cmd.subcommands;
    // At least "complete", "heartbeat", "pull" must appear.
    var has_complete = false;
    var has_heartbeat = false;
    var has_pull = false;
    for (subs) |s| {
        if (std.mem.eql(u8, s, "complete")) has_complete = true;
        if (std.mem.eql(u8, s, "heartbeat")) has_heartbeat = true;
        if (std.mem.eql(u8, s, "pull")) has_pull = true;
    }
    try std.testing.expect(has_complete);
    try std.testing.expect(has_heartbeat);
    try std.testing.expect(has_pull);
}

test "RawSchema: ignore_unknown_fields — extra keys at command and flag level do not cause ParseFailed" {
    // A fixture with extra unknown fields at both the command and flag level.
    // Must parse without error — proves schema drift tolerance.
    const fixture_extra =
        \\{"schemaVersion":1,"layout":"flat","root":"planar-agent",
        \\ "future_top_level_key":"ignored","commands":[
        \\   {"name":"complete","command":"planar-agent complete",
        \\    "subcommands":[],"hidden":false,"future_cmd_field":42,
        \\    "flags":[
        \\      {"long":"--claim","required":true,"description":"token",
        \\       "future_flag_field":"also-ignored"}
        \\    ],"positionals":[],"docs":{}}
        \\ ]}
    ;
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_extra, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    try std.testing.expectEqual(@as(u32, 1), parsed.value.schemaVersion);
    try std.testing.expectEqual(@as(usize, 1), parsed.value.commands.len);
    try std.testing.expectEqualStrings("--claim", parsed.value.commands[0].flags[0].long);
}

test "RawSchema: malformed JSON → ParseFailed error kind" {
    // Simulates what SubprocessFailed or a garbled response would produce.
    const bad = "{ not json {{{{";
    const result = std.json.parseFromSlice(RawSchema, std.testing.allocator, bad, .{
        .ignore_unknown_fields = true,
    });
    // Must return an error (any parse error); must NOT succeed.
    try std.testing.expectError(error.SyntaxError, result);
}

test "BinSchema: commands() returns all entries; findCommand returns correct entry" {
    // The queryable-surface accessor the brief compiler will use.
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    const bs = BinSchema.init(parsed.value);

    // root() returns the binary name.
    try std.testing.expectEqualStrings("planar-agent", bs.root());
    // schemaVersion()
    try std.testing.expectEqual(@as(u32, 1), bs.schemaVersion());

    // commands() covers the full flat list.
    try std.testing.expectEqual(@as(usize, 5), bs.commands().len);

    // findCommand returns the entry for "planar-agent complete".
    const cmd = bs.findCommand("planar-agent complete") orelse return error.TestUnexpectedNull;
    try std.testing.expectEqualStrings("planar-agent complete", cmd.command);
    try std.testing.expectEqual(@as(usize, 3), cmd.flags.len);
    try std.testing.expectEqualStrings("--claim", cmd.flags[0].long);
    try std.testing.expectEqual(true, cmd.flags[0].required);

    // findCommand returns null for unknown commands.
    try std.testing.expectEqual(@as(?*CommandEntry, null), bs.findCommand("planar-agent nonexistent"));
}

test "BinSchema: findCommand for 'planar-agent heartbeat' — --claim required, --ttl optional" {
    // Confirms the brief compiler can look up the heartbeat verb and its flags.
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    const bs = BinSchema.init(parsed.value);
    const cmd = bs.findCommand("planar-agent heartbeat") orelse return error.TestUnexpectedNull;

    try std.testing.expectEqualStrings("planar-agent heartbeat", cmd.command);
    try std.testing.expectEqual(@as(usize, 3), cmd.flags.len);

    // --claim is required.
    try std.testing.expectEqualStrings("--claim", cmd.flags[0].long);
    try std.testing.expect(cmd.flags[0].required);
    // --ttl is optional.
    try std.testing.expectEqualStrings("--ttl", cmd.flags[1].long);
    try std.testing.expect(!cmd.flags[1].required);
    // --json is optional.
    try std.testing.expectEqualStrings("--json", cmd.flags[2].long);
    try std.testing.expect(!cmd.flags[2].required);
}

test "BinSchema: commands() iteration yields correct command paths (brief compiler enumeration pattern)" {
    // Simulates the brief compiler's enumeration loop:
    //   for (bs.commands()) |cmd| { collect cmd.command + cmd.flags }
    const parsed = try std.json.parseFromSlice(RawSchema, std.testing.allocator, fixture_agent_schema, .{
        .ignore_unknown_fields = true,
    });
    defer parsed.deinit();

    const bs = BinSchema.init(parsed.value);

    // Collect all command paths.
    var paths = std.ArrayList([]const u8).empty;
    defer paths.deinit(std.testing.allocator);

    for (bs.commands()) |cmd| {
        try paths.append(std.testing.allocator, cmd.command);
    }

    // Must contain the core worker verbs.
    var found_complete = false;
    var found_heartbeat = false;
    var found_pull = false;
    var found_schema = false;
    for (paths.items) |p| {
        if (std.mem.eql(u8, p, "planar-agent complete")) found_complete = true;
        if (std.mem.eql(u8, p, "planar-agent heartbeat")) found_heartbeat = true;
        if (std.mem.eql(u8, p, "planar-agent pull")) found_pull = true;
        if (std.mem.eql(u8, p, "planar-agent schema")) found_schema = true;
    }
    try std.testing.expect(found_complete);
    try std.testing.expect(found_heartbeat);
    try std.testing.expect(found_pull);
    try std.testing.expect(found_schema);
}
