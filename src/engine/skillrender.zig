//! engine/skillrender — unified skills source renderer for Claude/Codex/Copilot.
//!
//! Mirrors Go's `src/internal/skillrender/*` surface:
//! - embedded vendor profile table from `src/configs/vendors.yaml`
//! - unified source parse from `skills/src/*.md`
//! - per-vendor markdown projection with ordered frontmatter fields
//! - tree render + drift check (`content` / `missing` / `orphan`)

const std = @import("std");
const config = @import("config.zig");
const model_engine = @import("models.zig");

pub const AgentsModelsPath = "agents/models.md";

/// Projection metadata contract. Both values are lowercase SHA-256 hex. The
/// source digest covers the parsed, path-independent authored source; the
/// projection digest additionally covers projection-relevant vendor inputs and
/// the metadata-free rendered payload. Bump these domains if the canonical
/// encoding changes.
const source_digest_domain = "planar-render-source-v1";
const projection_digest_domain = "planar-render-projection-v2";
pub const SourceDigestKey = "x-planar-source-digest";
pub const ProjectionDigestKey = "x-planar-projection-digest";

pub const ProjectionDigests = struct {
    source: [64]u8,
    projection: [64]u8,
};

const embedded_vendors_yaml =
    \\vendors:
    \\  claude:
    \\    title: Claude
    \\    output_dir: commands/claude
    \\    install_path: ~/.claude/commands/<slug>.md
    \\    invoke: /<slug>
    \\    has_invocation_block: true
    \\    frontmatter_fields: [description, argument-hint, model, source]
    \\    agents_output_dir: agents/claude
    \\    agent_format: md-yaml
    \\    agent_frontmatter_fields: [name, description, tools, model]
    \\    install_bullets:
    \\      - "Installed to `~/.claude/commands/<slug>.md`."
    \\      - "Invoked as `/<slug> <subcommand> [args]`."
    \\  codex:
    \\    title: Codex
    \\    output_dir: skills/codex
    \\    install_path: ~/.codex/skills/<slug>
    \\    invoke: <slug>
    \\    has_invocation_block: false
    \\    frontmatter_fields: [name, description, model, source]
    \\    agents_output_dir: agents/codex
    \\    agent_format: toml
    \\    agent_frontmatter_fields: [name, description, developer_instructions, model, model_reasoning_effort, sandbox_mode]
    \\    install_bullets:
    \\      - "Installed into `~/.codex/skills/<slug>` from `~/.planar/codex-skills/<slug>`."
    \\  copilot:
    \\    title: Copilot
    \\    output_dir: skills/copilot
    \\    install_path: ~/.copilot/skills/<slug>.md
    \\    invoke: <slug>
    \\    has_invocation_block: false
    \\    frontmatter_fields: [name, description, model, source]
    \\    agents_output_dir: agents/copilot
    \\    agent_format: md-yaml
    \\    agent_frontmatter_fields: [name, description, tools, model]
    \\    install_bullets:
    \\      - "Installed to `~/.copilot/skills/<slug>.md`."
    \\      - "Companion instruction and prompt files (when needed) live under `copilot/`."
;

pub const VendorModel = struct {
    tier: []const u8,
    model: []const u8,
};

pub const VendorProfile = struct {
    name: []const u8,
    title: []const u8,
    output_dir: []const u8,
    install_path: []const u8,
    invoke: []const u8,
    has_invocation_block: bool,
    frontmatter_fields: [][]const u8,
    install_bullets: [][]const u8,
    models: []VendorModel,
    /// Relative directory (under the render out-dir) for rendered agent
    /// role definitions. Empty when the vendor has no agent surface.
    agents_output_dir: []const u8 = "",
    /// Output projection for agent role files: `md-yaml` (Claude/Copilot)
    /// or `toml` (Codex).
    agent_format: []const u8 = "",
    /// Ordered frontmatter / body keys emitted for agent role files.
    agent_frontmatter_fields: [][]const u8 = &.{},
};

pub const VendorProfiles = struct {
    items: []VendorProfile,

    pub fn deinit(self: *VendorProfiles, allocator: std.mem.Allocator) void {
        for (self.items) |p| {
            allocator.free(p.name);
            allocator.free(p.title);
            allocator.free(p.output_dir);
            allocator.free(p.install_path);
            allocator.free(p.invoke);
            for (p.frontmatter_fields) |v| allocator.free(v);
            allocator.free(p.frontmatter_fields);
            for (p.install_bullets) |v| allocator.free(v);
            allocator.free(p.install_bullets);
            for (p.models) |m| {
                allocator.free(m.tier);
                allocator.free(m.model);
            }
            allocator.free(p.models);
            allocator.free(p.agents_output_dir);
            allocator.free(p.agent_format);
            for (p.agent_frontmatter_fields) |v| allocator.free(v);
            allocator.free(p.agent_frontmatter_fields);
        }
        allocator.free(self.items);
        self.* = .{ .items = &.{} };
    }

    pub fn get(self: VendorProfiles, name: []const u8) ?VendorProfile {
        for (self.items) |item| {
            if (std.mem.eql(u8, item.name, name)) return item;
        }
        return null;
    }

    pub fn namesSorted(self: VendorProfiles, allocator: std.mem.Allocator) ![][]const u8 {
        var out = try allocator.alloc([]const u8, self.items.len);
        for (self.items, 0..) |item, i| out[i] = item.name;
        std.mem.sort([]const u8, out, {}, lessString);
        return out;
    }
};

pub const SourceVendor = struct {
    name: []const u8,
    argument_hint: []const u8,
    invocation_examples: []const u8,
    model: []const u8,
};

pub const Source = struct {
    path: []const u8,
    slug: []const u8,
    description: []const u8,
    source_link: []const u8,
    model_tier: []const u8,
    vendor: []SourceVendor,
    shared_notes: [][]const u8,
    body: []const u8,

    pub fn deinit(self: *Source, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.slug);
        allocator.free(self.description);
        allocator.free(self.source_link);
        allocator.free(self.model_tier);
        for (self.vendor) |v| {
            allocator.free(v.name);
            allocator.free(v.argument_hint);
            allocator.free(v.invocation_examples);
            allocator.free(v.model);
        }
        allocator.free(self.vendor);
        for (self.shared_notes) |note| allocator.free(note);
        allocator.free(self.shared_notes);
        allocator.free(self.body);
        self.* = undefined;
    }
};

/// Parsed agent role spec from `agents/<role>.md`. Frontmatter fields are
/// `name, description, tier, role, capability`; the Markdown body becomes the
/// rendered subagent system prompt.
pub const AgentSource = struct {
    path: []const u8,
    name: []const u8,
    description: []const u8,
    tier: []const u8,
    role: []const u8,
    capability: []const u8,
    body: []const u8,

    pub fn deinit(self: *AgentSource, allocator: std.mem.Allocator) void {
        allocator.free(self.path);
        allocator.free(self.name);
        allocator.free(self.description);
        allocator.free(self.tier);
        allocator.free(self.role);
        allocator.free(self.capability);
        allocator.free(self.body);
        self.* = undefined;
    }
};

/// Non-role docs under `agents/` that carry no `role:`/`capability:`
/// frontmatter and must never be rendered as subagent definitions.
const agent_non_role_docs = [_][]const u8{
    "methodology.md",
    "models.md",
    "doctrine.md",
};

pub const RenderOptions = struct {
    src_dir: []const u8,
    out_dir: []const u8,
    slug_filter: []const []const u8 = &.{},
    vendors: ?VendorProfiles = null,
};

pub const RenderResult = struct {
    written_paths: [][]const u8,
    skipped_slugs: [][]const u8,

    pub fn deinit(self: *RenderResult, allocator: std.mem.Allocator) void {
        for (self.written_paths) |p| allocator.free(p);
        allocator.free(self.written_paths);
        for (self.skipped_slugs) |s| allocator.free(s);
        allocator.free(self.skipped_slugs);
        self.* = .{ .written_paths = &.{}, .skipped_slugs = &.{} };
    }
};

pub const CheckOptions = struct {
    src_dir: []const u8,
    out_dir: []const u8,
    slug_filter: []const []const u8 = &.{},
    emit_diff: bool = false,
    vendors: ?VendorProfiles = null,
};

pub const DriftReason = enum {
    content,
    missing,
    orphan,

    pub fn text(self: DriftReason) []const u8 {
        return switch (self) {
            .content => "content",
            .missing => "missing",
            .orphan => "orphan",
        };
    }
};

pub const DriftEntry = struct {
    path: []const u8,
    vendor: []const u8,
    slug: []const u8,
    reason: DriftReason,
};

pub const FileDiff = struct {
    path: []const u8,
    body: []const u8,
};

const ExpectedSlug = struct {
    vendor: []const u8,
    slug: []const u8,
};

pub const CheckResult = struct {
    drifted: []DriftEntry,
    diffs: []FileDiff,
    skipped_slugs: [][]const u8,

    pub fn inSync(self: CheckResult) bool {
        return self.drifted.len == 0;
    }

    pub fn diffForPath(self: CheckResult, path: []const u8) ?[]const u8 {
        for (self.diffs) |d| {
            if (std.mem.eql(u8, d.path, path)) return d.body;
        }
        return null;
    }

    pub fn deinit(self: *CheckResult, allocator: std.mem.Allocator) void {
        for (self.drifted) |d| {
            allocator.free(d.path);
            allocator.free(d.vendor);
            allocator.free(d.slug);
        }
        allocator.free(self.drifted);
        for (self.diffs) |d| {
            allocator.free(d.path);
            allocator.free(d.body);
        }
        allocator.free(self.diffs);
        for (self.skipped_slugs) |s| allocator.free(s);
        allocator.free(self.skipped_slugs);
        self.* = .{ .drifted = &.{}, .diffs = &.{}, .skipped_slugs = &.{} };
    }
};

pub const RenderError = error{
    MissingFrontmatter,
    MalformedFrontmatter,
    MissingSlug,
    MissingDescription,
    MissingSource,
    UnknownModelTier,
    UnknownFrontmatterField,
    UnknownTemplateToken,
    MissingTierTableHeading,
    ParseFailure,
    MissingAgentName,
    MissingAgentRole,
    MissingAgentCapability,
    MissingAgentTier,
    UnknownCapability,
    UnknownAgentFormat,
    UnknownAgentFrontmatterField,
};

pub fn loadVendors(allocator: std.mem.Allocator) !VendorProfiles {
    var profiles = try parseVendorsBytes(allocator, embedded_vendors_yaml);
    errdefer profiles.deinit(allocator);
    try populateModelsFromResolver(allocator, &profiles);
    return profiles;
}

/// Fill each vendor profile's tier→model table from the shared model resolver
/// (plan 540 phase 2, tasks 3622/3627) using config DEFAULTS — so the embedded
/// vendor YAML no longer carries a model table and `agents/models.md` + every
/// rendered skill/agent `model:` field flow from the single config source.
/// Replaces whatever `models` the YAML parse produced (now none).
fn populateModelsFromResolver(allocator: std.mem.Allocator, profiles: *VendorProfiles) !void {
    var resolved = config.resolve(allocator, null, std.process.Environ.empty, null) catch
        return RenderError.ParseFailure;
    defer resolved.deinit(allocator);

    const tiers = [_][]const u8{ "small", "medium", "large" };
    for (profiles.items) |*p| {
        // Discard any YAML-parsed models (none, post-retirement) and rebuild.
        for (p.models) |m| {
            allocator.free(m.tier);
            allocator.free(m.model);
        }
        allocator.free(p.models);
        p.models = &.{};

        var list: std.ArrayList(VendorModel) = .empty;
        errdefer {
            for (list.items) |m| {
                allocator.free(m.tier);
                allocator.free(m.model);
            }
            list.deinit(allocator);
        }
        for (tiers) |tier| {
            const r = model_engine.resolveTier(&resolved.effective, p.name, tier) catch continue;
            try list.append(allocator, .{
                .tier = try allocator.dupe(u8, tier),
                .model = try allocator.dupe(u8, r.model),
            });
        }
        p.models = try list.toOwnedSlice(allocator);
    }
}

pub fn parseVendorsBytes(allocator: std.mem.Allocator, raw: []const u8) !VendorProfiles {
    try validateFrontmatterSyntax(raw);
    var lines = splitLines(allocator, raw);
    defer lines.deinit(allocator);

    var i: usize = 0;
    var seen_root = false;
    while (i < lines.items.len) : (i += 1) {
        const line = trimSpace(lines.items[i]);
        if (line.len == 0) continue;
        if (std.mem.eql(u8, line, "vendors:")) {
            seen_root = true;
            i += 1;
            break;
        }
    }
    if (!seen_root) return RenderError.ParseFailure;

    var profiles: std.ArrayList(VendorProfile) = .empty;
    errdefer {
        var tmp = VendorProfiles{ .items = profiles.items };
        tmp.deinit(allocator);
        profiles = .empty;
    }

    while (i < lines.items.len) {
        const raw_line = lines.items[i];
        const t = trimSpace(raw_line);
        if (t.len == 0) {
            i += 1;
            continue;
        }
        if (indentWidth(raw_line) != 2 or !std.mem.endsWith(u8, t, ":")) {
            return RenderError.ParseFailure;
        }
        const vendor_name = trimSpace(t[0 .. t.len - 1]);
        var profile = VendorProfile{
            .name = try allocator.dupe(u8, vendor_name),
            .title = try allocator.dupe(u8, ""),
            .output_dir = try allocator.dupe(u8, ""),
            .install_path = try allocator.dupe(u8, ""),
            .invoke = try allocator.dupe(u8, ""),
            .has_invocation_block = false,
            .frontmatter_fields = &.{},
            .install_bullets = &.{},
            .models = &.{},
            .agents_output_dir = try allocator.dupe(u8, ""),
            .agent_format = try allocator.dupe(u8, ""),
            .agent_frontmatter_fields = &.{},
        };
        errdefer deinitVendorProfile(profile, allocator);
        i += 1;

        var frontmatter_fields = std.ArrayList([]const u8).empty;
        var install_bullets = std.ArrayList([]const u8).empty;
        var models = std.ArrayList(VendorModel).empty;
        var agent_frontmatter_fields = std.ArrayList([]const u8).empty;
        defer frontmatter_fields.deinit(allocator);
        defer install_bullets.deinit(allocator);
        defer models.deinit(allocator);
        defer agent_frontmatter_fields.deinit(allocator);

        while (i < lines.items.len) {
            const child_raw = lines.items[i];
            const child_trimmed = trimSpace(child_raw);
            if (child_trimmed.len == 0) {
                i += 1;
                continue;
            }
            const child_indent = indentWidth(child_raw);
            if (child_indent <= 2) break;
            if (child_indent != 4) return RenderError.ParseFailure;
            const colon = std.mem.indexOfScalar(u8, child_trimmed, ':') orelse return RenderError.ParseFailure;
            const key = trimSpace(child_trimmed[0..colon]);
            const val = trimSpace(child_trimmed[colon + 1 ..]);

            if (std.mem.eql(u8, key, "title")) {
                allocator.free(profile.title);
                profile.title = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "output_dir")) {
                allocator.free(profile.output_dir);
                profile.output_dir = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "install_path")) {
                allocator.free(profile.install_path);
                profile.install_path = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "invoke")) {
                allocator.free(profile.invoke);
                profile.invoke = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "has_invocation_block")) {
                profile.has_invocation_block = std.mem.eql(u8, try parseScalarValue(val), "true");
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "frontmatter_fields")) {
                const arr = try parseInlineList(allocator, val);
                defer {
                    for (arr) |item| allocator.free(item);
                    allocator.free(arr);
                }
                for (arr) |item| try frontmatter_fields.append(allocator, try allocator.dupe(u8, item));
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "agents_output_dir")) {
                allocator.free(profile.agents_output_dir);
                profile.agents_output_dir = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "agent_format")) {
                allocator.free(profile.agent_format);
                profile.agent_format = try dupScalarValue(allocator, val);
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "agent_frontmatter_fields")) {
                const arr = try parseInlineList(allocator, val);
                defer {
                    for (arr) |item| allocator.free(item);
                    allocator.free(arr);
                }
                for (arr) |item| try agent_frontmatter_fields.append(allocator, try allocator.dupe(u8, item));
                i += 1;
                continue;
            }
            if (std.mem.eql(u8, key, "install_bullets")) {
                i += 1;
                while (i < lines.items.len) {
                    const bullet_raw = lines.items[i];
                    const bullet_trim = trimSpace(bullet_raw);
                    if (bullet_trim.len == 0) {
                        i += 1;
                        continue;
                    }
                    const bullet_indent = indentWidth(bullet_raw);
                    if (bullet_indent <= 4) break;
                    if (bullet_indent != 6 or !std.mem.startsWith(u8, bullet_trim, "- ")) return RenderError.ParseFailure;
                    const note = try dupScalarValue(allocator, trimSpace(bullet_trim[2..]));
                    try install_bullets.append(allocator, note);
                    i += 1;
                }
                continue;
            }
            if (std.mem.eql(u8, key, "models")) {
                i += 1;
                while (i < lines.items.len) {
                    const model_raw = lines.items[i];
                    const model_trim = trimSpace(model_raw);
                    if (model_trim.len == 0) {
                        i += 1;
                        continue;
                    }
                    const model_indent = indentWidth(model_raw);
                    if (model_indent <= 4) break;
                    if (model_indent != 6) return RenderError.ParseFailure;
                    const mc = std.mem.indexOfScalar(u8, model_trim, ':') orelse return RenderError.ParseFailure;
                    const tier = try dupScalarValue(allocator, trimSpace(model_trim[0..mc]));
                    const model = try dupScalarValue(allocator, trimSpace(model_trim[mc + 1 ..]));
                    try models.append(allocator, .{
                        .tier = tier,
                        .model = model,
                    });
                    i += 1;
                }
                continue;
            }
            i += 1;
        }

        profile.frontmatter_fields = try frontmatter_fields.toOwnedSlice(allocator);
        profile.install_bullets = try install_bullets.toOwnedSlice(allocator);
        profile.models = try models.toOwnedSlice(allocator);
        profile.agent_frontmatter_fields = try agent_frontmatter_fields.toOwnedSlice(allocator);
        try profiles.append(allocator, profile);
    }

    if (profiles.items.len == 0) return RenderError.ParseFailure;
    return .{ .items = try profiles.toOwnedSlice(allocator) };
}

pub fn parseSourceFile(allocator: std.mem.Allocator, path: []const u8) !Source {
    const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(4 * 1024 * 1024));
    defer allocator.free(raw);
    return parseSourceBytes(allocator, path, raw);
}

pub fn parseSourceBytes(allocator: std.mem.Allocator, path: []const u8, raw: []const u8) !Source {
    if (!std.mem.startsWith(u8, raw, "---\n")) return RenderError.MissingFrontmatter;
    const rest = raw[4..];
    var end = std.mem.indexOf(u8, rest, "\n---\n");
    var close_len: usize = 5;
    if (end == null and std.mem.endsWith(u8, rest, "\n---")) {
        end = rest.len - 4;
        close_len = 4;
    }
    if (end == null) return RenderError.MalformedFrontmatter;

    const yaml_block = rest[0..end.?];
    try validateFrontmatterSyntax(yaml_block);
    const after_close = end.? + close_len;
    const body_src = if (after_close < rest.len) rest[after_close..] else "";
    const body_trimmed = trimLeadingNewlines(body_src);

    var source = Source{
        .path = try allocator.dupe(u8, path),
        .slug = try allocator.dupe(u8, ""),
        .description = try allocator.dupe(u8, ""),
        .source_link = try allocator.dupe(u8, ""),
        .model_tier = try allocator.dupe(u8, ""),
        .vendor = &.{},
        .shared_notes = &.{},
        .body = try allocator.dupe(u8, body_trimmed),
    };
    errdefer source.deinit(allocator);

    var lines = splitLines(allocator, yaml_block);
    defer lines.deinit(allocator);

    var vendors = std.ArrayList(SourceVendor).empty;
    var shared_notes = std.ArrayList([]const u8).empty;
    defer vendors.deinit(allocator);
    defer shared_notes.deinit(allocator);

    var i: usize = 0;
    while (i < lines.items.len) {
        const line_raw = lines.items[i];
        const line = trimSpace(line_raw);
        if (line.len == 0) {
            i += 1;
            continue;
        }
        const indent = indentWidth(line_raw);
        if (indent != 0) {
            i += 1;
            continue;
        }

        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return RenderError.ParseFailure;
        const key = trimSpace(line[0..colon]);
        const val = trimSpace(line[colon + 1 ..]);

        if (std.mem.eql(u8, key, "slug")) {
            allocator.free(source.slug);
            source.slug = try dupScalarValue(allocator, val);
            i += 1;
            continue;
        }
        if (std.mem.eql(u8, key, "description")) {
            allocator.free(source.description);
            source.description = try dupScalarValue(allocator, val);
            i += 1;
            continue;
        }
        if (std.mem.eql(u8, key, "source")) {
            allocator.free(source.source_link);
            source.source_link = try dupScalarValue(allocator, val);
            i += 1;
            continue;
        }
        if (std.mem.eql(u8, key, "model_tier")) {
            allocator.free(source.model_tier);
            source.model_tier = try dupScalarValue(allocator, val);
            i += 1;
            continue;
        }
        if (std.mem.eql(u8, key, "shared_notes")) {
            i += 1;
            while (i < lines.items.len) {
                const note_raw = lines.items[i];
                const note = trimSpace(note_raw);
                if (note.len == 0) {
                    i += 1;
                    continue;
                }
                const note_indent = indentWidth(note_raw);
                if (note_indent <= 0) break;
                if (note_indent == 2 and std.mem.startsWith(u8, note, "- ")) {
                    const text = try dupScalarValue(allocator, trimSpace(note[2..]));
                    try shared_notes.append(allocator, text);
                    i += 1;
                    continue;
                }
                if (note_indent <= 2) break;
                i += 1;
            }
            continue;
        }
        if (std.mem.eql(u8, key, "vendor")) {
            i += 1;
            while (i < lines.items.len) {
                const vendor_raw = lines.items[i];
                const vendor_trim = trimSpace(vendor_raw);
                if (vendor_trim.len == 0) {
                    i += 1;
                    continue;
                }
                const vendor_indent = indentWidth(vendor_raw);
                if (vendor_indent <= 0) break;
                if (vendor_indent != 2 or !std.mem.endsWith(u8, vendor_trim, ":")) {
                    i += 1;
                    continue;
                }
                const vendor_name = trimSpace(vendor_trim[0 .. vendor_trim.len - 1]);
                var sv = SourceVendor{
                    .name = try allocator.dupe(u8, vendor_name),
                    .argument_hint = try allocator.dupe(u8, ""),
                    .invocation_examples = try allocator.dupe(u8, ""),
                    .model = try allocator.dupe(u8, ""),
                };
                errdefer {
                    allocator.free(sv.name);
                    allocator.free(sv.argument_hint);
                    allocator.free(sv.invocation_examples);
                    allocator.free(sv.model);
                }
                i += 1;
                while (i < lines.items.len) {
                    const kv_raw = lines.items[i];
                    const kv_trim = trimSpace(kv_raw);
                    if (kv_trim.len == 0) {
                        i += 1;
                        continue;
                    }
                    const kv_indent = indentWidth(kv_raw);
                    if (kv_indent <= 2) break;
                    if (kv_indent != 4) {
                        i += 1;
                        continue;
                    }
                    const kcolon = std.mem.indexOfScalar(u8, kv_trim, ':') orelse {
                        i += 1;
                        continue;
                    };
                    const k = trimSpace(kv_trim[0..kcolon]);
                    const v = trimSpace(kv_trim[kcolon + 1 ..]);
                    if (std.mem.eql(u8, k, "argument_hint")) {
                        allocator.free(sv.argument_hint);
                        sv.argument_hint = try dupScalarValue(allocator, v);
                        i += 1;
                        continue;
                    }
                    if (std.mem.eql(u8, k, "model")) {
                        allocator.free(sv.model);
                        sv.model = try dupScalarValue(allocator, v);
                        i += 1;
                        continue;
                    }
                    if (std.mem.eql(u8, k, "invocation_examples")) {
                        if (!std.mem.eql(u8, v, "|")) return RenderError.ParseFailure;
                        i += 1;
                        var block = std.ArrayList(u8).empty;
                        defer block.deinit(allocator);
                        while (i < lines.items.len) {
                            const b_raw = lines.items[i];
                            const b_indent = indentWidth(b_raw);
                            if (trimSpace(b_raw).len == 0 and b_indent >= 6) {
                                try block.append(allocator, '\n');
                                i += 1;
                                continue;
                            }
                            if (b_indent < 6) break;
                            const line_bytes = b_raw[6..];
                            try block.appendSlice(allocator, line_bytes);
                            try block.append(allocator, '\n');
                            i += 1;
                        }
                        allocator.free(sv.invocation_examples);
                        sv.invocation_examples = try block.toOwnedSlice(allocator);
                        continue;
                    }
                    i += 1;
                }
                try vendors.append(allocator, sv);
            }
            continue;
        }
        i += 1;
    }

    if (source.slug.len == 0) return RenderError.MissingSlug;
    if (source.description.len == 0) return RenderError.MissingDescription;
    if (source.source_link.len == 0) return RenderError.MissingSource;

    source.vendor = try vendors.toOwnedSlice(allocator);
    source.shared_notes = try shared_notes.toOwnedSlice(allocator);
    return source;
}

pub fn render(allocator: std.mem.Allocator, source: Source, profile: VendorProfile) ![]u8 {
    const model = try resolveModel(allocator, source, profile);
    defer allocator.free(model);

    const vendor_notes = try buildVendorNotes(allocator, source, profile);
    defer allocator.free(vendor_notes);
    const invocation = invocationBlock(source, profile);
    const body = try renderBodyTemplate(allocator, source.body, .{
        .vendor_title = profile.title,
        .vendor_notes = vendor_notes,
        .invocation_block = invocation,
    });
    defer allocator.free(body);

    const frontmatter = try renderFrontmatter(allocator, source, profile, model);
    defer allocator.free(frontmatter);

    var payload = std.ArrayList(u8).empty;
    defer payload.deinit(allocator);
    try payload.appendSlice(allocator, "---\n");
    try payload.appendSlice(allocator, frontmatter);
    try payload.appendSlice(allocator, "---\n\n");
    try payload.appendSlice(allocator, body);
    if (body.len == 0 or body[body.len - 1] != '\n') try payload.append(allocator, '\n');

    const digests = try skillProjectionDigests(allocator, source, profile, payload.items);
    return addMarkdownProjectionMetadata(allocator, payload.items, digests);
}

pub fn parseAgentSourceFile(allocator: std.mem.Allocator, path: []const u8) !AgentSource {
    const raw = try std.Io.Dir.cwd().readFileAlloc(fsIo(), path, allocator, std.Io.Limit.limited(4 * 1024 * 1024));
    defer allocator.free(raw);
    return parseAgentSourceBytes(allocator, path, raw);
}

pub fn parseAgentSourceBytes(allocator: std.mem.Allocator, path: []const u8, raw: []const u8) !AgentSource {
    if (!std.mem.startsWith(u8, raw, "---\n")) return RenderError.MissingFrontmatter;
    const rest = raw[4..];
    var end = std.mem.indexOf(u8, rest, "\n---\n");
    var close_len: usize = 5;
    if (end == null and std.mem.endsWith(u8, rest, "\n---")) {
        end = rest.len - 4;
        close_len = 4;
    }
    if (end == null) return RenderError.MalformedFrontmatter;

    const yaml_block = rest[0..end.?];
    try validateFrontmatterSyntax(yaml_block);
    const after_close = end.? + close_len;
    const body_src = if (after_close < rest.len) rest[after_close..] else "";
    const body_trimmed = trimLeadingNewlines(body_src);

    var source = AgentSource{
        .path = try allocator.dupe(u8, path),
        .name = try allocator.dupe(u8, ""),
        .description = try allocator.dupe(u8, ""),
        .tier = try allocator.dupe(u8, ""),
        .role = try allocator.dupe(u8, ""),
        .capability = try allocator.dupe(u8, ""),
        .body = try allocator.dupe(u8, body_trimmed),
    };
    errdefer source.deinit(allocator);

    var lines = splitLines(allocator, yaml_block);
    defer lines.deinit(allocator);

    var i: usize = 0;
    while (i < lines.items.len) : (i += 1) {
        const line_raw = lines.items[i];
        const line = trimSpace(line_raw);
        if (line.len == 0) continue;
        if (indentWidth(line_raw) != 0) continue;
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return RenderError.ParseFailure;
        const key = trimSpace(line[0..colon]);
        const val = trimSpace(line[colon + 1 ..]);
        if (std.mem.eql(u8, key, "name")) {
            allocator.free(source.name);
            source.name = try dupScalarValue(allocator, val);
        } else if (std.mem.eql(u8, key, "description")) {
            allocator.free(source.description);
            source.description = try dupScalarValue(allocator, val);
        } else if (std.mem.eql(u8, key, "tier")) {
            allocator.free(source.tier);
            source.tier = try dupScalarValue(allocator, val);
        } else if (std.mem.eql(u8, key, "role")) {
            allocator.free(source.role);
            source.role = try dupScalarValue(allocator, val);
        } else if (std.mem.eql(u8, key, "capability")) {
            allocator.free(source.capability);
            source.capability = try dupScalarValue(allocator, val);
        }
    }

    if (source.name.len == 0) return RenderError.MissingAgentName;
    if (source.role.len == 0) return RenderError.MissingAgentRole;
    if (source.capability.len == 0) return RenderError.MissingAgentCapability;
    if (source.tier.len == 0) return RenderError.MissingAgentTier;
    return source;
}

/// Tools allowlist for the Claude/Copilot `tools:` frontmatter field, keyed by
/// the neutral capability enum (ADR #260 / decision #343). Order is fixed so the
/// rendered output is deterministic.
fn capabilityTools(capability: []const u8) RenderError![]const []const u8 {
    if (std.mem.eql(u8, capability, "coordinate")) {
        return &.{ "Bash", "Agent", "Read", "Grep", "Glob" };
    }
    if (std.mem.eql(u8, capability, "write")) {
        return &.{ "Read", "Edit", "Write", "Bash", "Grep", "Glob" };
    }
    if (std.mem.eql(u8, capability, "read-only")) {
        return &.{ "Read", "Grep", "Glob" };
    }
    return RenderError.UnknownCapability;
}

/// Codex `sandbox_mode` for the neutral capability enum. `coordinate` maps to
/// `workspace-write` (read-only would block legitimate `~/.planar` DB writes);
/// the weak-enforcement caveat is documented in the agent body (see ADR #260).
fn capabilitySandboxMode(capability: []const u8) RenderError![]const u8 {
    if (std.mem.eql(u8, capability, "coordinate")) return "workspace-write";
    if (std.mem.eql(u8, capability, "write")) return "workspace-write";
    if (std.mem.eql(u8, capability, "read-only")) return "read-only";
    return RenderError.UnknownCapability;
}

/// Codex `model_reasoning_effort` derived from the model tier. `large` tiers
/// reason at `high`; everything else at `medium`.
fn reasoningEffortForTier(tier: []const u8) []const u8 {
    if (std.mem.eql(u8, tier, "large")) return "high";
    return "medium";
}

const coordinate_codex_note =
    "> Codex enforcement caveat: this role is `coordinate` — it runs CLI commands and spawns\n" ++
    "> subagents but must NOT edit source files. Codex's `sandbox_mode` is a coarse\n" ++
    "> filesystem-write switch and cannot express that boundary precisely; `workspace-write` is\n" ++
    "> set so legitimate `planar`/`planar-agent` DB writes succeed. Do not edit repository source\n" ++
    "> files from this agent — that boundary is doctrinal here, not structurally enforced.\n";

/// resolveAgentModel maps the agent's tier to a concrete model via the vendor's
/// existing `models:` table.
fn resolveAgentModel(allocator: std.mem.Allocator, source: AgentSource, profile: VendorProfile) ![]u8 {
    if (source.tier.len == 0) return allocator.dupe(u8, "");
    for (profile.models) |m| {
        if (std.mem.eql(u8, m.tier, source.tier)) return allocator.dupe(u8, m.model);
    }
    return RenderError.UnknownModelTier;
}

fn skillProjectionDigests(
    allocator: std.mem.Allocator,
    source: Source,
    profile: VendorProfile,
    payload: []const u8,
) !ProjectionDigests {
    var canonical = std.ArrayList(u8).empty;
    defer canonical.deinit(allocator);
    try appendDigestField(allocator, &canonical, "domain", source_digest_domain);
    try appendDigestField(allocator, &canonical, "kind", "skill");
    try appendDigestField(allocator, &canonical, "slug", source.slug);
    try appendDigestField(allocator, &canonical, "description", source.description);
    try appendDigestField(allocator, &canonical, "source_link", source.source_link);
    try appendDigestField(allocator, &canonical, "model_tier", source.model_tier);
    for (source.vendor, 0..) |vendor, i| {
        try appendIndexedDigestField(allocator, &canonical, "vendor_name", i, vendor.name);
        try appendIndexedDigestField(allocator, &canonical, "vendor_argument_hint", i, vendor.argument_hint);
        try appendIndexedDigestField(allocator, &canonical, "vendor_invocation_examples", i, vendor.invocation_examples);
        try appendIndexedDigestField(allocator, &canonical, "vendor_model", i, vendor.model);
    }
    for (source.shared_notes, 0..) |note, i| {
        try appendIndexedDigestField(allocator, &canonical, "shared_note", i, note);
    }
    try appendDigestField(allocator, &canonical, "body", source.body);
    const source_digest = sha256Hex(canonical.items);

    return .{
        .source = source_digest,
        .projection = try projectionDigestForPayload(allocator, "skill", &source_digest, profile.name, payload),
    };
}

fn agentProjectionDigests(
    allocator: std.mem.Allocator,
    source: AgentSource,
    profile: VendorProfile,
    payload: []const u8,
) !ProjectionDigests {
    var canonical = std.ArrayList(u8).empty;
    defer canonical.deinit(allocator);
    try appendDigestField(allocator, &canonical, "domain", source_digest_domain);
    try appendDigestField(allocator, &canonical, "kind", "agent");
    try appendDigestField(allocator, &canonical, "name", source.name);
    try appendDigestField(allocator, &canonical, "description", source.description);
    try appendDigestField(allocator, &canonical, "tier", source.tier);
    try appendDigestField(allocator, &canonical, "role", source.role);
    try appendDigestField(allocator, &canonical, "capability", source.capability);
    try appendDigestField(allocator, &canonical, "body", source.body);
    const source_digest = sha256Hex(canonical.items);

    return .{
        .source = source_digest,
        .projection = try projectionDigestForPayload(allocator, "agent", &source_digest, profile.name, payload),
    };
}

/// Recomputes the projection digest from the metadata-free bytes that are
/// installed. Vendor-profile changes are represented by their rendered payload,
/// allowing status checks to detect semantic edits without the authored source.
pub fn projectionDigestForPayload(
    allocator: std.mem.Allocator,
    kind: []const u8,
    source_digest: []const u8,
    vendor: []const u8,
    payload: []const u8,
) ![64]u8 {
    if ((!std.mem.eql(u8, kind, "skill") and !std.mem.eql(u8, kind, "agent")) or
        source_digest.len != 64 or vendor.len == 0)
    {
        return RenderError.ParseFailure;
    }
    var canonical = std.ArrayList(u8).empty;
    defer canonical.deinit(allocator);
    try appendDigestField(allocator, &canonical, "domain", projection_digest_domain);
    try appendDigestField(allocator, &canonical, "kind", kind);
    try appendDigestField(allocator, &canonical, "source_digest", source_digest);
    try appendDigestField(allocator, &canonical, "vendor", vendor);
    try appendDigestField(allocator, &canonical, "payload", payload);
    return sha256Hex(canonical.items);
}

/// Canonical field encoding is `label N:value\n`, where N is the byte length
/// of value. Length-prefixing keeps arbitrary Markdown unambiguous; field order
/// is fixed above and never comes from filesystem traversal.
fn appendDigestField(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(u8),
    label: []const u8,
    value: []const u8,
) !void {
    const prefix = try std.fmt.allocPrint(allocator, "{s} {d}:", .{ label, value.len });
    defer allocator.free(prefix);
    try out.appendSlice(allocator, prefix);
    try out.appendSlice(allocator, value);
    try out.append(allocator, '\n');
}

fn appendIndexedDigestField(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(u8),
    label: []const u8,
    index: usize,
    value: []const u8,
) !void {
    const indexed = try std.fmt.allocPrint(allocator, "{s}[{d}]", .{ label, index });
    defer allocator.free(indexed);
    try appendDigestField(allocator, out, indexed, value);
}

fn sha256Hex(bytes: []const u8) [64]u8 {
    var digest: [std.crypto.hash.sha2.Sha256.digest_length]u8 = undefined;
    std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
    return std.fmt.bytesToHex(digest, .lower);
}

fn addMarkdownProjectionMetadata(
    allocator: std.mem.Allocator,
    payload: []const u8,
    digests: ProjectionDigests,
) ![]u8 {
    const close = std.mem.indexOf(u8, payload, "\n---\n") orelse return RenderError.MalformedFrontmatter;
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    try out.appendSlice(allocator, payload[0 .. close + 1]);
    const metadata = try std.fmt.allocPrint(
        allocator,
        "{s}: {s}\n{s}: {s}\n",
        .{ SourceDigestKey, digests.source, ProjectionDigestKey, digests.projection },
    );
    defer allocator.free(metadata);
    try out.appendSlice(allocator, metadata);
    try out.appendSlice(allocator, payload[close + 1 ..]);
    return out.toOwnedSlice(allocator);
}

fn addTomlProjectionMetadata(
    allocator: std.mem.Allocator,
    payload: []const u8,
    digests: ProjectionDigests,
) ![]u8 {
    return std.fmt.allocPrint(
        allocator,
        "# {s}: {s}\n# {s}: {s}\n{s}",
        .{ SourceDigestKey, digests.source, ProjectionDigestKey, digests.projection, payload },
    );
}

/// renderAgent projects one agent role spec into its per-vendor file body.
/// Claude/Copilot emit YAML frontmatter + body; Codex emits a TOML document with
/// the body inlined as a triple-quoted `developer_instructions` string.
pub fn renderAgent(allocator: std.mem.Allocator, source: AgentSource, profile: VendorProfile) ![]u8 {
    const model = try resolveAgentModel(allocator, source, profile);
    defer allocator.free(model);

    const payload = if (std.mem.eql(u8, profile.agent_format, "md-yaml"))
        try renderAgentMdYaml(allocator, source, profile, model)
    else if (std.mem.eql(u8, profile.agent_format, "toml"))
        try renderAgentToml(allocator, source, profile, model)
    else
        return RenderError.UnknownAgentFormat;
    defer allocator.free(payload);

    const digests = try agentProjectionDigests(allocator, source, profile, payload);
    if (std.mem.eql(u8, profile.agent_format, "md-yaml")) {
        return addMarkdownProjectionMetadata(allocator, payload, digests);
    }
    if (std.mem.eql(u8, profile.agent_format, "toml")) {
        return addTomlProjectionMetadata(allocator, payload, digests);
    }
    unreachable;
}

fn renderAgentMdYaml(
    allocator: std.mem.Allocator,
    source: AgentSource,
    profile: VendorProfile,
    model: []const u8,
) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    try out.appendSlice(allocator, "---\n");
    for (profile.agent_frontmatter_fields) |field| {
        if (std.mem.eql(u8, field, "name")) {
            const line = try std.fmt.allocPrint(allocator, "name: {s}\n", .{source.name});
            defer allocator.free(line);
            try out.appendSlice(allocator, line);
        } else if (std.mem.eql(u8, field, "description")) {
            try appendYamlSingleQuotedKeyValue(allocator, &out, "description", source.description);
        } else if (std.mem.eql(u8, field, "tools")) {
            const tools = try capabilityTools(source.capability);
            const joined = try joinFlowList(allocator, tools);
            defer allocator.free(joined);
            const line = try std.fmt.allocPrint(allocator, "tools: {s}\n", .{joined});
            defer allocator.free(line);
            try out.appendSlice(allocator, line);
        } else if (std.mem.eql(u8, field, "model")) {
            if (model.len > 0) {
                const line = try std.fmt.allocPrint(allocator, "model: {s}\n", .{model});
                defer allocator.free(line);
                try out.appendSlice(allocator, line);
            }
        } else {
            return RenderError.UnknownAgentFrontmatterField;
        }
    }
    try out.appendSlice(allocator, "---\n\n");
    try out.appendSlice(allocator, source.body);
    if (source.body.len == 0 or source.body[source.body.len - 1] != '\n') try out.append(allocator, '\n');
    return out.toOwnedSlice(allocator);
}

fn renderAgentToml(
    allocator: std.mem.Allocator,
    source: AgentSource,
    profile: VendorProfile,
    model: []const u8,
) ![]u8 {
    const sandbox_mode = try capabilitySandboxMode(source.capability);
    const is_coordinate = std.mem.eql(u8, source.capability, "coordinate");

    // The body becomes developer_instructions. For coordinate roles, prepend
    // the weak-enforcement caveat so the model has the doctrinal boundary.
    var body_buf = std.ArrayList(u8).empty;
    defer body_buf.deinit(allocator);
    if (is_coordinate) {
        try body_buf.appendSlice(allocator, coordinate_codex_note);
        try body_buf.append(allocator, '\n');
    }
    try body_buf.appendSlice(allocator, source.body);
    if (body_buf.items.len == 0 or body_buf.items[body_buf.items.len - 1] != '\n') {
        try body_buf.append(allocator, '\n');
    }

    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    for (profile.agent_frontmatter_fields) |field| {
        if (std.mem.eql(u8, field, "name")) {
            try appendTomlKeyValue(allocator, &out, "name", source.name);
        } else if (std.mem.eql(u8, field, "description")) {
            try appendTomlKeyValue(allocator, &out, "description", source.description);
        } else if (std.mem.eql(u8, field, "model")) {
            if (model.len > 0) try appendTomlKeyValue(allocator, &out, "model", model);
        } else if (std.mem.eql(u8, field, "model_reasoning_effort")) {
            try appendTomlKeyValue(allocator, &out, "model_reasoning_effort", reasoningEffortForTier(source.tier));
        } else if (std.mem.eql(u8, field, "sandbox_mode")) {
            try appendTomlKeyValue(allocator, &out, "sandbox_mode", sandbox_mode);
        } else if (std.mem.eql(u8, field, "developer_instructions")) {
            // Use TOML multi-line basic string ("""). Backslash escapes ARE
            // processed, so we must escape `\` as `\\`. A literal `"""` in the
            // body would prematurely close the string; per the TOML spec the
            // sequence can be written as `""\` (escaped-quote trick) which the
            // parser reassembles as three unescaped quotes. We do NOT use
            // multi-line literal strings (''') because they cannot contain ''' —
            // same class of problem — and markdown bodies are far more likely to
            // hold triple-double-quotes (Python docstrings, JSON examples) than
            // triple-single-quotes.
            try out.appendSlice(allocator, "developer_instructions = \"\"\"\n");
            try appendTomlMlbsBody(allocator, &out, body_buf.items);
            try out.appendSlice(allocator, "\"\"\"\n");
        } else {
            return RenderError.UnknownAgentFrontmatterField;
        }
    }
    return out.toOwnedSlice(allocator);
}

/// joinFlowList renders a YAML inline (flow) sequence: `[a, b, c]`.
fn joinFlowList(allocator: std.mem.Allocator, items: []const []const u8) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    try out.append(allocator, '[');
    for (items, 0..) |item, idx| {
        if (idx > 0) try out.appendSlice(allocator, ", ");
        try out.appendSlice(allocator, item);
    }
    try out.append(allocator, ']');
    return out.toOwnedSlice(allocator);
}

/// appendTomlMlbsBody writes `body` into an already-opened TOML multi-line basic
/// string (the caller has already emitted `"""\n`). Escapes that keep the emitted
/// TOML valid for arbitrary prose:
///   `\`   → `\\`   (MLBS processes backslash escapes; literal backslash must be doubled)
///   `"""` → `""\"` (three literal quotes would close the string; the escaped-quote
///                   trick reassembles to three unescaped quotes on parse)
fn appendTomlMlbsBody(allocator: std.mem.Allocator, out: *std.ArrayList(u8), body: []const u8) !void {
    var i: usize = 0;
    while (i < body.len) {
        if (body[i] == '\\') {
            try out.appendSlice(allocator, "\\\\");
            i += 1;
        } else if (i + 2 < body.len and body[i] == '"' and body[i + 1] == '"' and body[i + 2] == '"') {
            // Emit the first two quotes literally; escape the third so the
            // parser sees \" and reconstructs three unescaped quotes.
            try out.appendSlice(allocator, "\"\"\\\"");
            i += 3;
        } else {
            try out.append(allocator, body[i]);
            i += 1;
        }
    }
}

/// appendTomlKeyValue appends `key = "value"\n` to `out`, escaping the value as
/// a basic TOML string.
fn appendTomlKeyValue(allocator: std.mem.Allocator, out: *std.ArrayList(u8), key: []const u8, value: []const u8) !void {
    const quoted = try tomlString(allocator, value);
    defer allocator.free(quoted);
    try out.appendSlice(allocator, key);
    try out.appendSlice(allocator, " = ");
    try out.appendSlice(allocator, quoted);
    try out.append(allocator, '\n');
}

/// appendYamlSingleQuotedKeyValue appends `key: 'value'\n`, escaping embedded
/// single quotes per YAML single-quoted scalar rules. Skill descriptions are
/// prose and can contain `: `, which is invalid as an unquoted plain scalar.
fn appendYamlSingleQuotedKeyValue(
    allocator: std.mem.Allocator,
    out: *std.ArrayList(u8),
    key: []const u8,
    value: []const u8,
) !void {
    try out.appendSlice(allocator, key);
    try out.appendSlice(allocator, ": '");
    for (value) |c| {
        switch (c) {
            '\'' => try out.appendSlice(allocator, "''"),
            '\n', '\r' => return RenderError.ParseFailure,
            else => try out.append(allocator, c),
        }
    }
    try out.appendSlice(allocator, "'\n");
}

/// tomlString renders a basic single-line TOML string literal with the standard
/// escapes. Returns a caller-owned slice.
fn tomlString(allocator: std.mem.Allocator, value: []const u8) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    try out.append(allocator, '"');
    for (value) |c| {
        switch (c) {
            '"' => try out.appendSlice(allocator, "\\\""),
            '\\' => try out.appendSlice(allocator, "\\\\"),
            '\n' => try out.appendSlice(allocator, "\\n"),
            '\t' => try out.appendSlice(allocator, "\\t"),
            '\r' => try out.appendSlice(allocator, "\\r"),
            else => try out.append(allocator, c),
        }
    }
    try out.append(allocator, '"');
    return out.toOwnedSlice(allocator);
}

/// isAgentRoleFile returns true when an `agents/` entry is a renderable role
/// spec (a `.md` file that is not one of the non-role doctrine docs).
fn isAgentRoleFile(name: []const u8) bool {
    if (!std.mem.endsWith(u8, name, ".md")) return false;
    for (agent_non_role_docs) |doc| {
        if (std.mem.eql(u8, name, doc)) return false;
    }
    return true;
}

/// listAgentSources enumerates renderable agent role files directly under
/// `agents_dir` (the `<out_dir>/agents` directory). Link-mode installs stage
/// canonical role sources as symlinks inside a real, prefix-owned agents
/// directory, so both regular files and symlinks are valid inputs. Returns an
/// empty slice when the directory does not exist.
fn listAgentSources(allocator: std.mem.Allocator, agents_dir: []const u8) ![][]const u8 {
    var dir = std.Io.Dir.cwd().openDir(fsIo(), agents_dir, .{ .iterate = true }) catch |e| switch (e) {
        error.FileNotFound => return allocator.alloc([]const u8, 0),
        else => return e,
    };
    defer dir.close(fsIo());
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (entry.kind != .file and entry.kind != .sym_link) continue;
        if (!isAgentRoleFile(entry.name)) continue;
        const joined = try std.fs.path.join(allocator, &.{ agents_dir, entry.name });
        try out.append(allocator, joined);
    }
    std.mem.sort([]const u8, out.items, {}, lessString);
    return out.toOwnedSlice(allocator);
}

/// agentOutputName returns the rendered file name for a role under a vendor's
/// agents_output_dir: `<role>.agent.md` for Copilot, `<role>.toml` for Codex,
/// `<role>.md` otherwise (Claude).
fn agentOutputName(allocator: std.mem.Allocator, role: []const u8, profile: VendorProfile) ![]u8 {
    if (std.mem.eql(u8, profile.name, "copilot")) {
        return std.fmt.allocPrint(allocator, "{s}.agent.md", .{role});
    }
    if (std.mem.eql(u8, profile.agent_format, "toml")) {
        return std.fmt.allocPrint(allocator, "{s}.toml", .{role});
    }
    return std.fmt.allocPrint(allocator, "{s}.md", .{role});
}

pub fn renderTree(allocator: std.mem.Allocator, opts: RenderOptions) !RenderResult {
    const vendors_owned = if (opts.vendors) |v| v else try loadVendors(allocator);
    var vendors = vendors_owned;
    defer if (opts.vendors == null) vendors.deinit(allocator);

    const source_paths = try listSources(allocator, opts.src_dir);
    defer freeStringSlice(allocator, source_paths);

    var parsed = std.ArrayList(Source).empty;
    defer {
        for (parsed.items) |*s| s.deinit(allocator);
        parsed.deinit(allocator);
    }
    for (source_paths) |path| {
        try parsed.append(allocator, try parseSourceFile(allocator, path));
    }

    var pending = std.ArrayList(struct { dst: []const u8, bytes: []u8 }).empty;
    defer {
        for (pending.items) |p| {
            allocator.free(p.dst);
            allocator.free(p.bytes);
        }
        pending.deinit(allocator);
    }
    var skipped = std.ArrayList([]const u8).empty;
    defer {
        for (skipped.items) |s| allocator.free(s);
        skipped.deinit(allocator);
    }

    const names = try vendors.namesSorted(allocator);
    defer allocator.free(names);

    for (parsed.items) |src| {
        if (!slugIncluded(src.slug, opts.slug_filter)) {
            try skipped.append(allocator, try allocator.dupe(u8, src.slug));
            continue;
        }
        for (names) |vendor_name| {
            const profile = vendors.get(vendor_name).?;
            const bytes = try render(allocator, src, profile);
            const rel_dir = outputDir(profile);
            const rel_name = try std.fmt.allocPrint(allocator, "{s}.md", .{src.slug});
            defer allocator.free(rel_name);
            const dst = try std.fs.path.join(allocator, &.{ opts.out_dir, rel_dir, rel_name });
            try pending.append(allocator, .{ .dst = dst, .bytes = bytes });
        }
    }

    const agents_dir = try std.fs.path.join(allocator, &.{ opts.out_dir, "agents" });
    defer allocator.free(agents_dir);

    // Agent role render: walk <out_dir>/agents/*.md role specs (skipping the
    // non-role doctrine docs) and project each per vendor into agents_output_dir.
    // The vendor agent dirs (agents/<vendor>/) are written into the SAME pending
    // list, but the models.md tier-table patch below keys on `agents_dir`
    // (== <out_dir>/agents) existing — which is the agent SOURCE dir, not a
    // vendor subdir we create. Writes are flushed only after this whole block,
    // so creating agents/<vendor>/ never causes the models.md patch to
    // double-fire within a single renderTree call.
    {
        const agent_paths = try listAgentSources(allocator, agents_dir);
        defer freeStringSlice(allocator, agent_paths);
        for (agent_paths) |apath| {
            var asrc = try parseAgentSourceFile(allocator, apath);
            defer asrc.deinit(allocator);
            if (!slugIncluded(asrc.name, opts.slug_filter)) continue;
            for (names) |vendor_name| {
                const profile = vendors.get(vendor_name).?;
                if (profile.agents_output_dir.len == 0) continue;
                const bytes = try renderAgent(allocator, asrc, profile);
                const rel_name = try agentOutputName(allocator, asrc.name, profile);
                defer allocator.free(rel_name);
                const dst = try std.fs.path.join(allocator, &.{ opts.out_dir, profile.agents_output_dir, rel_name });
                try pending.append(allocator, .{ .dst = dst, .bytes = bytes });
            }
        }
    }

    if (pathIsDir(agents_dir)) {
        const models_path = try std.fs.path.join(allocator, &.{ opts.out_dir, AgentsModelsPath });
        defer allocator.free(models_path);
        const current = try std.Io.Dir.cwd().readFileAlloc(fsIo(), models_path, allocator, std.Io.Limit.limited(4 * 1024 * 1024));
        defer allocator.free(current);
        const rewritten = try renderModelsDoc(allocator, current, vendors);
        const dst = try allocator.dupe(u8, models_path);
        try pending.append(allocator, .{ .dst = dst, .bytes = rewritten });
    }

    var written = std.ArrayList([]const u8).empty;
    errdefer {
        for (written.items) |w| allocator.free(w);
        written.deinit(allocator);
    }
    for (pending.items) |p| {
        if (std.fs.path.dirname(p.dst)) |parent| {
            try std.Io.Dir.cwd().createDirPath(fsIo(), parent);
        }
        try std.Io.Dir.cwd().writeFile(fsIo(), .{
            .sub_path = p.dst,
            .data = p.bytes,
        });
        try written.append(allocator, try allocator.dupe(u8, p.dst));
    }

    const written_slice = try written.toOwnedSlice(allocator);
    std.mem.sort([]const u8, written_slice, {}, lessString);
    const skipped_slice = try skipped.toOwnedSlice(allocator);
    std.mem.sort([]const u8, skipped_slice, {}, lessString);
    return .{
        .written_paths = written_slice,
        .skipped_slugs = skipped_slice,
    };
}

pub fn checkTree(allocator: std.mem.Allocator, opts: CheckOptions) !CheckResult {
    const vendors_owned = if (opts.vendors) |v| v else try loadVendors(allocator);
    var vendors = vendors_owned;
    defer if (opts.vendors == null) vendors.deinit(allocator);

    const source_paths = listSources(allocator, opts.src_dir) catch |e| switch (e) {
        error.FileNotFound => blk: {
            break :blk try allocator.alloc([]const u8, 0);
        },
        else => return e,
    };
    defer freeStringSlice(allocator, source_paths);

    var parsed = std.ArrayList(Source).empty;
    defer {
        for (parsed.items) |*s| s.deinit(allocator);
        parsed.deinit(allocator);
    }
    for (source_paths) |path| {
        try parsed.append(allocator, try parseSourceFile(allocator, path));
    }

    var drift = std.ArrayList(DriftEntry).empty;
    errdefer {
        for (drift.items) |d| allocator.free(d.path);
        drift.deinit(allocator);
    }
    var diffs = std.ArrayList(FileDiff).empty;
    errdefer {
        for (diffs.items) |d| {
            allocator.free(d.path);
            allocator.free(d.body);
        }
        diffs.deinit(allocator);
    }
    var skipped = std.ArrayList([]const u8).empty;
    errdefer {
        for (skipped.items) |s| allocator.free(s);
        skipped.deinit(allocator);
    }

    const names = try vendors.namesSorted(allocator);
    defer allocator.free(names);
    var expected = std.ArrayList(ExpectedSlug).empty;
    defer expected.deinit(allocator);

    for (parsed.items) |src| {
        if (!slugIncluded(src.slug, opts.slug_filter)) {
            try skipped.append(allocator, try allocator.dupe(u8, src.slug));
            continue;
        }
        for (names) |vendor_name| {
            const profile = vendors.get(vendor_name).?;
            const rel_dir = outputDir(profile);
            const rel_file = try std.fmt.allocPrint(allocator, "{s}.md", .{src.slug});
            defer allocator.free(rel_file);
            const rel = try std.fs.path.join(allocator, &.{ rel_dir, rel_file });
            defer allocator.free(rel);
            try expected.append(allocator, .{ .vendor = vendor_name, .slug = src.slug });

            const rendered = try render(allocator, src, profile);
            defer allocator.free(rendered);
            const dst = try std.fs.path.join(allocator, &.{ opts.out_dir, rel });
            defer allocator.free(dst);

            const on_disk = std.Io.Dir.cwd().readFileAlloc(fsIo(), dst, allocator, std.Io.Limit.limited(4 * 1024 * 1024)) catch |e| switch (e) {
                error.FileNotFound => null,
                else => return e,
            };
            if (on_disk == null) {
                try drift.append(allocator, .{
                    .path = try allocator.dupe(u8, rel),
                    .vendor = try allocator.dupe(u8, vendor_name),
                    .slug = try allocator.dupe(u8, src.slug),
                    .reason = .missing,
                });
                continue;
            }
            defer allocator.free(on_disk.?);
            if (!std.mem.eql(u8, rendered, on_disk.?)) {
                try drift.append(allocator, .{
                    .path = try allocator.dupe(u8, rel),
                    .vendor = try allocator.dupe(u8, vendor_name),
                    .slug = try allocator.dupe(u8, src.slug),
                    .reason = .content,
                });
                if (opts.emit_diff) {
                    const body = try unifiedDiff(allocator, rel, on_disk.?, rendered);
                    try diffs.append(allocator, .{
                        .path = try allocator.dupe(u8, rel),
                        .body = body,
                    });
                }
            }
        }
    }

    if (opts.slug_filter.len == 0) {
        for (names) |vendor_name| {
            const profile = vendors.get(vendor_name).?;
            const dir_rel = outputDir(profile);
            const dir_abs = try std.fs.path.join(allocator, &.{ opts.out_dir, dir_rel });
            defer allocator.free(dir_abs);
            var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_abs, .{ .iterate = true }) catch |e| switch (e) {
                error.FileNotFound => continue,
                else => return e,
            };
            defer dir.close(fsIo());
            var it = dir.iterate();
            while (try it.next(fsIo())) |entry| {
                if (entry.kind != .file) continue;
                if (!std.mem.endsWith(u8, entry.name, ".md")) continue;
                const stem = entry.name[0 .. entry.name.len - 3];
                if (isExpected(expected.items, vendor_name, stem)) continue;
                const rel = try std.fs.path.join(allocator, &.{ dir_rel, entry.name });
                defer allocator.free(rel);
                try drift.append(allocator, .{
                    .path = try allocator.dupe(u8, rel),
                    .vendor = try allocator.dupe(u8, vendor_name),
                    .slug = try allocator.dupe(u8, stem),
                    .reason = .orphan,
                });
            }
        }
    }

    const agents_dir = try std.fs.path.join(allocator, &.{ opts.out_dir, "agents" });
    defer allocator.free(agents_dir);

    // Agent render-target drift: content/missing for each vendor's rendered
    // role file, plus orphan detection in each agents_output_dir. The expected
    // set is (vendor, rendered-filename) because the extensions differ per
    // vendor (.md / .agent.md / .toml).
    {
        var expected_agents = std.ArrayList(ExpectedSlug).empty;
        defer {
            for (expected_agents.items) |e| allocator.free(e.slug);
            expected_agents.deinit(allocator);
        }
        const agent_paths = listAgentSources(allocator, agents_dir) catch |e| switch (e) {
            error.FileNotFound => try allocator.alloc([]const u8, 0),
            else => return e,
        };
        defer freeStringSlice(allocator, agent_paths);
        for (agent_paths) |apath| {
            var asrc = try parseAgentSourceFile(allocator, apath);
            defer asrc.deinit(allocator);
            if (!slugIncluded(asrc.name, opts.slug_filter)) continue;
            for (names) |vendor_name| {
                const profile = vendors.get(vendor_name).?;
                if (profile.agents_output_dir.len == 0) continue;
                const rel_file = try agentOutputName(allocator, asrc.name, profile);
                defer allocator.free(rel_file);
                const rel = try std.fs.path.join(allocator, &.{ profile.agents_output_dir, rel_file });
                defer allocator.free(rel);
                try expected_agents.append(allocator, .{
                    .vendor = vendor_name,
                    .slug = try allocator.dupe(u8, rel_file),
                });

                const rendered = try renderAgent(allocator, asrc, profile);
                defer allocator.free(rendered);
                const dst = try std.fs.path.join(allocator, &.{ opts.out_dir, rel });
                defer allocator.free(dst);
                const on_disk = std.Io.Dir.cwd().readFileAlloc(fsIo(), dst, allocator, std.Io.Limit.limited(4 * 1024 * 1024)) catch |e| switch (e) {
                    error.FileNotFound => null,
                    else => return e,
                };
                if (on_disk == null) {
                    try drift.append(allocator, .{
                        .path = try allocator.dupe(u8, rel),
                        .vendor = try allocator.dupe(u8, vendor_name),
                        .slug = try allocator.dupe(u8, asrc.name),
                        .reason = .missing,
                    });
                    continue;
                }
                defer allocator.free(on_disk.?);
                if (!std.mem.eql(u8, rendered, on_disk.?)) {
                    try drift.append(allocator, .{
                        .path = try allocator.dupe(u8, rel),
                        .vendor = try allocator.dupe(u8, vendor_name),
                        .slug = try allocator.dupe(u8, asrc.name),
                        .reason = .content,
                    });
                    if (opts.emit_diff) {
                        const body = try unifiedDiff(allocator, rel, on_disk.?, rendered);
                        try diffs.append(allocator, .{
                            .path = try allocator.dupe(u8, rel),
                            .body = body,
                        });
                    }
                }
            }
        }

        if (opts.slug_filter.len == 0) {
            for (names) |vendor_name| {
                const profile = vendors.get(vendor_name).?;
                if (profile.agents_output_dir.len == 0) continue;
                const dir_abs = try std.fs.path.join(allocator, &.{ opts.out_dir, profile.agents_output_dir });
                defer allocator.free(dir_abs);
                var dir = std.Io.Dir.cwd().openDir(fsIo(), dir_abs, .{ .iterate = true }) catch |e| switch (e) {
                    error.FileNotFound => continue,
                    else => return e,
                };
                defer dir.close(fsIo());
                var it = dir.iterate();
                while (try it.next(fsIo())) |entry| {
                    if (entry.kind != .file) continue;
                    if (isExpected(expected_agents.items, vendor_name, entry.name)) continue;
                    const rel = try std.fs.path.join(allocator, &.{ profile.agents_output_dir, entry.name });
                    defer allocator.free(rel);
                    try drift.append(allocator, .{
                        .path = try allocator.dupe(u8, rel),
                        .vendor = try allocator.dupe(u8, vendor_name),
                        .slug = try allocator.dupe(u8, entry.name),
                        .reason = .orphan,
                    });
                }
            }
        }
    }

    if (pathIsDir(agents_dir)) {
        const models_path = try std.fs.path.join(allocator, &.{ opts.out_dir, AgentsModelsPath });
        defer allocator.free(models_path);
        const current = std.Io.Dir.cwd().readFileAlloc(fsIo(), models_path, allocator, std.Io.Limit.limited(4 * 1024 * 1024)) catch |e| switch (e) {
            error.FileNotFound => null,
            else => return e,
        };
        if (current == null) {
            try drift.append(allocator, .{
                .path = try allocator.dupe(u8, AgentsModelsPath),
                .vendor = try allocator.dupe(u8, "agents"),
                .slug = try allocator.dupe(u8, ""),
                .reason = .missing,
            });
        } else {
            defer allocator.free(current.?);
            const expected_models = try renderModelsDoc(allocator, current.?, vendors);
            defer allocator.free(expected_models);
            if (!std.mem.eql(u8, expected_models, current.?)) {
                try drift.append(allocator, .{
                    .path = try allocator.dupe(u8, AgentsModelsPath),
                    .vendor = try allocator.dupe(u8, "agents"),
                    .slug = try allocator.dupe(u8, ""),
                    .reason = .content,
                });
                if (opts.emit_diff) {
                    const body = try unifiedDiff(allocator, AgentsModelsPath, current.?, expected_models);
                    try diffs.append(allocator, .{
                        .path = try allocator.dupe(u8, AgentsModelsPath),
                        .body = body,
                    });
                }
            }
        }
    }

    std.mem.sort(DriftEntry, drift.items, {}, lessDrift);
    const drift_slice = try drift.toOwnedSlice(allocator);
    const diff_slice = try diffs.toOwnedSlice(allocator);
    const skipped_slice = try skipped.toOwnedSlice(allocator);
    std.mem.sort([]const u8, skipped_slice, {}, lessString);

    return .{
        .drifted = drift_slice,
        .diffs = diff_slice,
        .skipped_slugs = skipped_slice,
    };
}

fn renderFrontmatter(
    allocator: std.mem.Allocator,
    source: Source,
    profile: VendorProfile,
    model: []const u8,
) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    for (profile.frontmatter_fields) |field| {
        if (std.mem.eql(u8, field, "name")) {
            const line = try std.fmt.allocPrint(allocator, "name: {s}\n", .{source.slug});
            defer allocator.free(line);
            try out.appendSlice(allocator, line);
            continue;
        }
        if (std.mem.eql(u8, field, "description")) {
            try appendYamlSingleQuotedKeyValue(allocator, &out, "description", source.description);
            continue;
        }
        if (std.mem.eql(u8, field, "source")) {
            const line = try std.fmt.allocPrint(allocator, "source: {s}\n", .{source.source_link});
            defer allocator.free(line);
            try out.appendSlice(allocator, line);
            continue;
        }
        if (std.mem.eql(u8, field, "model")) {
            if (model.len > 0) {
                const line = try std.fmt.allocPrint(allocator, "model: {s}\n", .{model});
                defer allocator.free(line);
                try out.appendSlice(allocator, line);
            }
            continue;
        }
        if (std.mem.eql(u8, field, "argument-hint")) {
            const hint = claudeArgumentHint(source);
            if (hint.len > 0) {
                const line = try std.fmt.allocPrint(allocator, "argument-hint: {s}\n", .{hint});
                defer allocator.free(line);
                try out.appendSlice(allocator, line);
            }
            continue;
        }
        return RenderError.UnknownFrontmatterField;
    }
    return out.toOwnedSlice(allocator);
}

fn resolveModel(allocator: std.mem.Allocator, source: Source, profile: VendorProfile) ![]u8 {
    if (sourceVendor(source, profile.name)) |vendor| {
        if (vendor.model.len > 0) return allocator.dupe(u8, vendor.model);
    }
    if (source.model_tier.len == 0) return allocator.dupe(u8, "");
    for (profile.models) |m| {
        if (std.mem.eql(u8, m.tier, source.model_tier)) return allocator.dupe(u8, m.model);
    }
    return RenderError.UnknownModelTier;
}

fn buildVendorNotes(allocator: std.mem.Allocator, source: Source, profile: VendorProfile) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    for (profile.install_bullets) |bullet| {
        const substituted = try replaceSlug(allocator, bullet, source.slug);
        defer allocator.free(substituted);
        try out.appendSlice(allocator, "- ");
        try out.appendSlice(allocator, substituted);
        try out.append(allocator, '\n');
    }
    for (source.shared_notes) |note| {
        try out.appendSlice(allocator, "- ");
        try out.appendSlice(allocator, note);
        try out.append(allocator, '\n');
    }
    while (out.items.len > 0 and out.items[out.items.len - 1] == '\n') {
        _ = out.pop();
    }
    return out.toOwnedSlice(allocator);
}

fn sourceVendor(source: Source, name: []const u8) ?SourceVendor {
    for (source.vendor) |v| {
        if (std.mem.eql(u8, v.name, name)) return v;
    }
    return null;
}

fn claudeArgumentHint(source: Source) []const u8 {
    if (sourceVendor(source, "claude")) |v| return v.argument_hint;
    return "";
}

fn invocationBlock(source: Source, profile: VendorProfile) []const u8 {
    if (!profile.has_invocation_block) return "";
    if (sourceVendor(source, profile.name)) |v| return v.invocation_examples;
    return "";
}

const TemplateValues = struct {
    vendor_title: []const u8,
    vendor_notes: []const u8,
    invocation_block: []const u8,
};

fn renderBodyTemplate(allocator: std.mem.Allocator, body: []const u8, values: TemplateValues) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);

    var condition_stack = std.ArrayList(bool).empty;
    defer condition_stack.deinit(allocator);
    try condition_stack.append(allocator, true);

    var i: usize = 0;
    while (i < body.len) {
        if (i + 1 < body.len and body[i] == '{' and body[i + 1] == '{') {
            var j = i + 2;
            var left_trim = false;
            if (j < body.len and body[j] == '-') {
                left_trim = true;
                j += 1;
            }
            const close = std.mem.indexOfPos(u8, body, j, "}}") orelse return RenderError.UnknownTemplateToken;
            var right_trim = false;
            var expr_end = close;
            if (expr_end > j and body[expr_end - 1] == '-') {
                right_trim = true;
                expr_end -= 1;
            }
            const expr = trimSpace(body[j..expr_end]);
            if (left_trim) trimRightWhitespace(&out);

            const active = allTrue(condition_stack.items);
            if (std.mem.eql(u8, expr, "if .InvocationBlock")) {
                const cond = values.invocation_block.len > 0;
                try condition_stack.append(allocator, active and cond);
            } else if (std.mem.eql(u8, expr, "end")) {
                if (condition_stack.items.len <= 1) return RenderError.UnknownTemplateToken;
                _ = condition_stack.pop();
            } else if (std.mem.eql(u8, expr, ".VendorTitle")) {
                if (active) try out.appendSlice(allocator, values.vendor_title);
            } else if (std.mem.eql(u8, expr, ".VendorNotes")) {
                if (active) try out.appendSlice(allocator, values.vendor_notes);
            } else if (std.mem.eql(u8, expr, ".InvocationBlock")) {
                if (active) try out.appendSlice(allocator, values.invocation_block);
            } else if (std.mem.eql(u8, expr, "\"{{\"")) {
                if (active) try out.appendSlice(allocator, "{{");
            } else {
                return RenderError.UnknownTemplateToken;
            }

            i = close + 2;
            if (right_trim) {
                while (i < body.len and isWhitespace(body[i])) : (i += 1) {}
            }
            continue;
        }
        if (allTrue(condition_stack.items)) try out.append(allocator, body[i]);
        i += 1;
    }

    if (condition_stack.items.len != 1) return RenderError.UnknownTemplateToken;
    return out.toOwnedSlice(allocator);
}

fn renderModelsDoc(allocator: std.mem.Allocator, current: []const u8, vendors: VendorProfiles) ![]u8 {
    var lines = splitLines(allocator, current);
    defer lines.deinit(allocator);
    // splitScalar on input ending in '\n' yields a trailing empty
    // string item ("a\nb\n" → ["a","b",""]). The line-by-line
    // append loop below treats every item as a line + '\n', which
    // would convert the phantom trailing "" into a real extra
    // newline. Each render pass would then grow the file by one
    // blank line — a non-idempotent renderer bug. Drop the
    // trailing empty before processing; we restore the trailing
    // newline at the end based on the source's original state.
    if (lines.items.len > 0 and lines.items[lines.items.len - 1].len == 0) {
        _ = lines.pop();
    }
    var heading_idx: ?usize = null;
    for (lines.items, 0..) |line, i| {
        if (std.mem.eql(u8, trimSpace(line), "## Tier Table")) {
            heading_idx = i;
            break;
        }
    }
    if (heading_idx == null) return RenderError.MissingTierTableHeading;
    var start = heading_idx.? + 1;
    while (start < lines.items.len and trimSpace(lines.items[start]).len == 0) : (start += 1) {}
    var end = start;
    while (end < lines.items.len) : (end += 1) {
        if (std.mem.startsWith(u8, trimSpace(lines.items[end]), "## ")) break;
    }

    const table_lines = try buildTierTableLines(allocator, vendors);
    defer freeStringSlice(allocator, table_lines);

    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    for (lines.items[0..start]) |line| {
        try out.appendSlice(allocator, line);
        try out.append(allocator, '\n');
    }
    for (table_lines) |line| {
        try out.appendSlice(allocator, line);
        try out.append(allocator, '\n');
    }
    for (lines.items[end..]) |line| {
        try out.appendSlice(allocator, line);
        try out.append(allocator, '\n');
    }
    if (current.len > 0 and current[current.len - 1] != '\n' and out.items.len > 0 and out.items[out.items.len - 1] == '\n') {
        _ = out.pop();
    }
    return out.toOwnedSlice(allocator);
}

fn buildTierTableLines(allocator: std.mem.Allocator, vendors: VendorProfiles) ![][]const u8 {
    const names = try vendors.namesSorted(allocator);
    defer allocator.free(names);
    const tiers = try orderedTierKeys(allocator, vendors);
    defer freeStringSlice(allocator, tiers);

    var header = std.ArrayList([]const u8).empty;
    defer header.deinit(allocator);
    var sep = std.ArrayList([]const u8).empty;
    defer {
        for (sep.items) |item| allocator.free(item);
        sep.deinit(allocator);
    }
    try header.append(allocator, "Tier");
    try sep.append(allocator, try allocator.dupe(u8, "------"));
    for (names) |n| {
        const p = vendors.get(n).?;
        const title = if (p.title.len > 0) p.title else p.name;
        try header.append(allocator, title);
        const dash = try repeatDash(allocator, title.len);
        defer allocator.free(dash);
        try sep.append(allocator, try allocator.dupe(u8, dash));
    }

    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |line| allocator.free(line);
        out.deinit(allocator);
    }
    const hline = try joinPipeRow(allocator, header.items);
    try out.append(allocator, hline);
    const sline = try joinPipeRow(allocator, sep.items);
    try out.append(allocator, sline);
    for (tiers) |tier| {
        var row = std.ArrayList([]const u8).empty;
        defer row.deinit(allocator);
        try row.append(allocator, tier);
        for (names) |n| {
            const p = vendors.get(n).?;
            const m = modelForTier(p, tier) orelse "";
            try row.append(allocator, m);
        }
        const line = try joinPipeRow(allocator, row.items);
        try out.append(allocator, line);
    }
    return out.toOwnedSlice(allocator);
}

fn orderedTierKeys(allocator: std.mem.Allocator, vendors: VendorProfiles) ![][]const u8 {
    var set = std.ArrayList([]const u8).empty;
    defer set.deinit(allocator);
    for (vendors.items) |p| {
        for (p.models) |m| {
            if (!containsString(set.items, m.tier)) try set.append(allocator, m.tier);
        }
    }

    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |s| allocator.free(s);
        out.deinit(allocator);
    }
    var rest = std.ArrayList([]const u8).empty;
    defer {
        for (rest.items) |s| allocator.free(s);
        rest.deinit(allocator);
    }
    const preferred = [_][]const u8{ "small", "medium", "large" };
    for (preferred) |tier| {
        if (containsString(set.items, tier)) try out.append(allocator, try allocator.dupe(u8, tier));
    }
    for (set.items) |tier| {
        if (std.mem.eql(u8, tier, "small") or std.mem.eql(u8, tier, "medium") or std.mem.eql(u8, tier, "large")) continue;
        try rest.append(allocator, try allocator.dupe(u8, tier));
    }
    std.mem.sort([]const u8, rest.items, {}, lessString);
    for (rest.items) |tier| try out.append(allocator, try allocator.dupe(u8, tier));
    return out.toOwnedSlice(allocator);
}

fn unifiedDiff(allocator: std.mem.Allocator, path: []const u8, old_text: []const u8, new_text: []const u8) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    var old_list = splitLines(allocator, old_text);
    const old_lines = try old_list.toOwnedSlice(allocator);
    defer allocator.free(old_lines);
    var new_list = splitLines(allocator, new_text);
    const new_lines = try new_list.toOwnedSlice(allocator);
    defer allocator.free(new_lines);
    const h1 = try std.fmt.allocPrint(allocator, "--- a/{s}\n", .{path});
    defer allocator.free(h1);
    try out.appendSlice(allocator, h1);
    const h2 = try std.fmt.allocPrint(allocator, "+++ b/{s}\n", .{path});
    defer allocator.free(h2);
    try out.appendSlice(allocator, h2);
    const h3 = try std.fmt.allocPrint(allocator, "@@ -1,{d} +1,{d} @@\n", .{ old_lines.len, new_lines.len });
    defer allocator.free(h3);
    try out.appendSlice(allocator, h3);
    for (old_lines) |line| {
        const row = try std.fmt.allocPrint(allocator, "-{s}\n", .{line});
        defer allocator.free(row);
        try out.appendSlice(allocator, row);
    }
    for (new_lines) |line| {
        const row = try std.fmt.allocPrint(allocator, "+{s}\n", .{line});
        defer allocator.free(row);
        try out.appendSlice(allocator, row);
    }
    return out.toOwnedSlice(allocator);
}

fn listSources(allocator: std.mem.Allocator, dir_path: []const u8) ![][]const u8 {
    var dir = try std.Io.Dir.cwd().openDir(fsIo(), dir_path, .{ .iterate = true });
    defer dir.close(fsIo());
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |p| allocator.free(p);
        out.deinit(allocator);
    }
    var it = dir.iterate();
    while (try it.next(fsIo())) |entry| {
        if (entry.kind != .file) continue;
        if (!std.mem.endsWith(u8, entry.name, ".md")) continue;
        const joined = try std.fs.path.join(allocator, &.{ dir_path, entry.name });
        try out.append(allocator, joined);
    }
    std.mem.sort([]const u8, out.items, {}, lessString);
    return out.toOwnedSlice(allocator);
}

fn parseInlineList(allocator: std.mem.Allocator, value: []const u8) ![][]const u8 {
    const v = trimSpace(value);
    if (v.len < 2 or v[0] != '[' or v[v.len - 1] != ']') return RenderError.ParseFailure;
    var inner = std.mem.splitScalar(u8, v[1 .. v.len - 1], ',');
    var out = std.ArrayList([]const u8).empty;
    errdefer {
        for (out.items) |s| allocator.free(s);
        out.deinit(allocator);
    }
    while (inner.next()) |piece| {
        const p = try dupScalarValue(allocator, trimSpace(piece));
        if (p.len == 0) {
            allocator.free(p);
            continue;
        }
        try out.append(allocator, p);
    }
    return out.toOwnedSlice(allocator);
}

fn dupScalarValue(allocator: std.mem.Allocator, s: []const u8) ![]const u8 {
    const t = trimSpace(s);
    const v = try parseScalarValue(t);
    if (t.len >= 2 and t[0] == '"' and t[t.len - 1] == '"') {
        return unescapeDoubleQuoted(allocator, v);
    }
    if (t.len >= 2 and t[0] == '\'' and t[t.len - 1] == '\'') {
        return unescapeSingleQuoted(allocator, v);
    }
    return allocator.dupe(u8, v);
}

fn unescapeSingleQuoted(allocator: std.mem.Allocator, inner: []const u8) ![]const u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    var i: usize = 0;
    while (i < inner.len) {
        if (inner[i] == '\'' and i + 1 < inner.len and inner[i + 1] == '\'') {
            try out.append(allocator, '\'');
            i += 2;
            continue;
        }
        try out.append(allocator, inner[i]);
        i += 1;
    }
    return out.toOwnedSlice(allocator);
}

fn unescapeDoubleQuoted(allocator: std.mem.Allocator, inner: []const u8) ![]const u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    var i: usize = 0;
    while (i < inner.len) {
        if (inner[i] != '\\') {
            try out.append(allocator, inner[i]);
            i += 1;
            continue;
        }
        i += 1;
        if (i >= inner.len) return RenderError.ParseFailure;
        switch (inner[i]) {
            '"' => try out.append(allocator, '"'),
            '\\' => try out.append(allocator, '\\'),
            'n' => try out.append(allocator, '\n'),
            'r' => try out.append(allocator, '\r'),
            't' => try out.append(allocator, '\t'),
            'b' => try out.append(allocator, 0x08),
            'f' => try out.append(allocator, 0x0c),
            '0' => try out.append(allocator, 0x00),
            '/' => try out.append(allocator, '/'),
            else => return RenderError.ParseFailure,
        }
        i += 1;
    }
    return out.toOwnedSlice(allocator);
}

fn splitLines(allocator: std.mem.Allocator, input: []const u8) std.ArrayList([]const u8) {
    var out = std.ArrayList([]const u8).empty;
    var it = std.mem.splitScalar(u8, input, '\n');
    while (it.next()) |line| {
        out.append(allocator, line) catch unreachable;
    }
    return out;
}

fn trimLeadingNewlines(in: []const u8) []const u8 {
    var start: usize = 0;
    while (start < in.len and (in[start] == '\n' or in[start] == '\r')) : (start += 1) {}
    return in[start..];
}

fn parseScalarValue(s: []const u8) ![]const u8 {
    const v = trimSpace(s);
    if (v.len == 0) return v;
    if (v[0] == '"' or v[0] == '\'') {
        const q = v[0];
        if (v.len < 2 or v[v.len - 1] != q) return RenderError.ParseFailure;
        var i: usize = 1;
        while (i + 1 < v.len) : (i += 1) {
            if (v[i] != q) continue;
            if (q == '"' and i > 0 and v[i - 1] == '\\') continue;
            if (q == '\'' and i + 1 < v.len - 1 and v[i + 1] == '\'') {
                i += 1;
                continue;
            }
            return RenderError.ParseFailure;
        }
        return v[1 .. v.len - 1];
    }
    if (v[0] == '[') {
        if (v[v.len - 1] != ']') return RenderError.ParseFailure;
    }
    if (v[0] == '{') {
        if (v[v.len - 1] != '}') return RenderError.ParseFailure;
    }
    return v;
}

fn trimSpace(s: []const u8) []const u8 {
    return std.mem.trim(u8, s, " \t\r");
}

fn validateFrontmatterSyntax(raw: []const u8) !void {
    var lines = std.mem.splitScalar(u8, raw, '\n');
    var block_indent: ?usize = null;
    while (lines.next()) |line_raw| {
        const line = trimSpace(line_raw);
        const indent = indentWidth(line_raw);
        if (block_indent) |need| {
            if (line.len == 0) continue;
            if (indent >= need) continue;
            block_indent = null;
        }
        if (line.len == 0) continue;
        if (line[0] == '#') continue;
        if (std.mem.startsWith(u8, line, "- ")) {
            const item = trimSpace(line[2..]);
            if (item.len == 0) continue;
            if (std.mem.indexOfScalar(u8, item, ':')) |idx| {
                _ = try parseScalarValue(trimSpace(item[idx + 1 ..]));
            } else {
                _ = try parseScalarValue(item);
            }
            continue;
        }
        const colon = std.mem.indexOfScalar(u8, line, ':') orelse return RenderError.ParseFailure;
        const value = trimSpace(line[colon + 1 ..]);
        if (value.len == 0) continue;
        if (std.mem.eql(u8, value, "|") or std.mem.eql(u8, value, ">")) {
            block_indent = indent + 2;
            continue;
        }
        _ = try parseScalarValue(value);
    }
}

fn indentWidth(s: []const u8) usize {
    var i: usize = 0;
    while (i < s.len and s[i] == ' ') : (i += 1) {}
    return i;
}

fn lessString(_: void, a: []const u8, b: []const u8) bool {
    return std.mem.order(u8, a, b) == .lt;
}

fn lessDrift(_: void, a: DriftEntry, b: DriftEntry) bool {
    return lessString({}, a.path, b.path);
}

fn repeatDash(allocator: std.mem.Allocator, n: usize) ![]const u8 {
    const out = try allocator.alloc(u8, n);
    @memset(out, '-');
    return out;
}

fn containsString(haystack: []const []const u8, needle: []const u8) bool {
    for (haystack) |v| if (std.mem.eql(u8, v, needle)) return true;
    return false;
}

fn slugIncluded(slug: []const u8, filter: []const []const u8) bool {
    if (filter.len == 0) return true;
    return containsString(filter, slug);
}

fn outputDir(profile: VendorProfile) []const u8 {
    return if (profile.output_dir.len > 0) profile.output_dir else profile.name;
}

/// Reads one digest value from either YAML frontmatter (`key: value`) or a
/// leading TOML comment (`# key: value`). The returned slice borrows `bytes`.
pub fn projectionMetadataValue(bytes: []const u8, key: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, bytes, '\n');
    while (lines.next()) |raw_line| {
        var line = trimSpace(raw_line);
        if (std.mem.startsWith(u8, line, "# ")) line = trimSpace(line[2..]);
        if (!std.mem.startsWith(u8, line, key)) continue;
        if (line.len <= key.len or line[key.len] != ':') continue;
        const value = trimSpace(line[key.len + 1 ..]);
        if (value.len == 64 and isLowerHex(value)) return value;
    }
    return null;
}

fn isLowerHex(value: []const u8) bool {
    for (value) |c| {
        if (!std.ascii.isDigit(c) and !(c >= 'a' and c <= 'f')) return false;
    }
    return true;
}

fn modelForTier(profile: VendorProfile, tier: []const u8) ?[]const u8 {
    for (profile.models) |m| if (std.mem.eql(u8, m.tier, tier)) return m.model;
    return null;
}

fn joinPipeRow(allocator: std.mem.Allocator, cols: []const []const u8) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    try out.appendSlice(allocator, "| ");
    for (cols, 0..) |c, i| {
        if (i > 0) try out.appendSlice(allocator, " | ");
        try out.appendSlice(allocator, c);
    }
    try out.appendSlice(allocator, " |");
    return out.toOwnedSlice(allocator);
}

fn replaceSlug(allocator: std.mem.Allocator, text: []const u8, slug: []const u8) ![]u8 {
    var out = std.ArrayList(u8).empty;
    errdefer out.deinit(allocator);
    var i: usize = 0;
    while (i < text.len) {
        if (i + "<slug>".len <= text.len and std.mem.eql(u8, text[i .. i + "<slug>".len], "<slug>")) {
            try out.appendSlice(allocator, slug);
            i += "<slug>".len;
        } else {
            try out.append(allocator, text[i]);
            i += 1;
        }
    }
    return out.toOwnedSlice(allocator);
}

fn trimRightWhitespace(out: *std.ArrayList(u8)) void {
    while (out.items.len > 0 and isWhitespace(out.items[out.items.len - 1])) {
        _ = out.pop();
    }
}

fn isWhitespace(c: u8) bool {
    return c == ' ' or c == '\t' or c == '\n' or c == '\r';
}

fn allTrue(items: []const bool) bool {
    for (items) |v| if (!v) return false;
    return true;
}

fn deinitVendorProfile(profile: VendorProfile, allocator: std.mem.Allocator) void {
    allocator.free(profile.name);
    allocator.free(profile.title);
    allocator.free(profile.output_dir);
    allocator.free(profile.install_path);
    allocator.free(profile.invoke);
    for (profile.frontmatter_fields) |f| allocator.free(f);
    allocator.free(profile.frontmatter_fields);
    for (profile.install_bullets) |b| allocator.free(b);
    allocator.free(profile.install_bullets);
    for (profile.models) |m| {
        allocator.free(m.tier);
        allocator.free(m.model);
    }
    allocator.free(profile.models);
    allocator.free(profile.agents_output_dir);
    allocator.free(profile.agent_format);
    for (profile.agent_frontmatter_fields) |f| allocator.free(f);
    allocator.free(profile.agent_frontmatter_fields);
}

fn freeStringSlice(allocator: std.mem.Allocator, vals: []const []const u8) void {
    for (vals) |v| allocator.free(v);
    allocator.free(vals);
}

fn pathIsDir(path: []const u8) bool {
    var dir = std.Io.Dir.cwd().openDir(fsIo(), path, .{}) catch return false;
    defer dir.close(fsIo());
    return true;
}

fn isExpected(expected: []const ExpectedSlug, vendor: []const u8, slug: []const u8) bool {
    for (expected) |e| {
        if (std.mem.eql(u8, e.vendor, vendor) and std.mem.eql(u8, e.slug, slug)) return true;
    }
    return false;
}

fn fsIo() std.Io {
    return std.Io.Threaded.global_single_threaded.io();
}

test "load vendors includes expected profiles" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    try std.testing.expect(vendors.get("claude") != null);
    try std.testing.expect(vendors.get("codex") != null);
    try std.testing.expect(vendors.get("copilot") != null);
    try std.testing.expect(vendors.get("claude").?.has_invocation_block);
    try std.testing.expectEqualStrings("commands/claude", vendors.get("claude").?.output_dir);
}

test "parse source required fields" {
    const gpa = std.testing.allocator;
    const raw =
        \\---
        \\slug: pl-plan
        \\description: Draft a plan
        \\source: docs/cli-reference.md#domain-plan
        \\model_tier: large
        \\vendor:
        \\  claude:
        \\    argument_hint: "<create|show>"
        \\    invocation_examples: |
        \\      /pl-plan create "x"
        \\shared_notes:
        \\  - from cli
        \\---
        \\
        \\# {{.VendorTitle}}
        \\
        \\{{.VendorNotes}}
        \\{{- if .InvocationBlock}}
        \\{{.InvocationBlock -}}
        \\{{- end}}
    ;
    var src = try parseSourceBytes(gpa, "skills/src/pl-plan.md", raw);
    defer src.deinit(gpa);
    try std.testing.expectEqualStrings("pl-plan", src.slug);
    try std.testing.expectEqualStrings("large", src.model_tier);
    try std.testing.expectEqual(@as(usize, 1), src.vendor.len);
    try std.testing.expectEqual(@as(usize, 1), src.shared_notes.len);
}

test "render projects claude and codex frontmatter differences" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const raw =
        \\---
        \\slug: pl-plan
        \\description: Draft a plan from a goal.
        \\source: docs/cli-reference.md#domain-plan
        \\model_tier: large
        \\vendor:
        \\  claude:
        \\    argument_hint: "<create|show|list> [args]"
        \\    invocation_examples: |
        \\      /pl-plan create "x"
        \\shared_notes:
        \\  - shared note
        \\---
        \\
        \\# Planar Plan ({{.VendorTitle}})
        \\{{.VendorNotes}}
        \\{{- if .InvocationBlock}}
        \\{{.InvocationBlock -}}
        \\{{- end}}
    ;
    var src = try parseSourceBytes(gpa, "skills/src/pl-plan.md", raw);
    defer src.deinit(gpa);
    const claude = try render(gpa, src, vendors.get("claude").?);
    defer gpa.free(claude);
    const codex = try render(gpa, src, vendors.get("codex").?);
    defer gpa.free(codex);
    try std.testing.expect(std.mem.indexOf(u8, claude, "argument-hint: <create|show|list> [args]") != null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "argument-hint:") == null);
    try std.testing.expect(std.mem.indexOf(u8, codex, "name: pl-plan") != null);
}

test "skill projection digests are path-independent and vendor-sensitive" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const raw =
        \\---
        \\slug: pl-digest
        \\description: Digest fixture.
        \\source: docs/skill-reference.md
        \\model_tier: medium
        \\---
        \\
        \\# {{.VendorTitle}}
    ;
    var source_a = try parseSourceBytes(gpa, "/checkout-a/skills/src/pl-digest.md", raw);
    defer source_a.deinit(gpa);
    var source_b = try parseSourceBytes(gpa, "/checkout-b/skills/src/pl-digest.md", raw);
    defer source_b.deinit(gpa);

    const claude_a = try render(gpa, source_a, vendors.get("claude").?);
    defer gpa.free(claude_a);
    const claude_b = try render(gpa, source_b, vendors.get("claude").?);
    defer gpa.free(claude_b);
    try std.testing.expectEqualStrings(claude_a, claude_b);

    var changed_profile = vendors.get("claude").?;
    changed_profile.title = "Changed Claude";
    const changed = try render(gpa, source_a, changed_profile);
    defer gpa.free(changed);
    try std.testing.expectEqualStrings(
        projectionMetadataValue(claude_a, SourceDigestKey).?,
        projectionMetadataValue(changed, SourceDigestKey).?,
    );
    try std.testing.expect(!std.mem.eql(
        u8,
        projectionMetadataValue(claude_a, ProjectionDigestKey).?,
        projectionMetadataValue(changed, ProjectionDigestKey).?,
    ));
}

test "renderTree slug filter writes only selected slug" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "skills-render-filter" });
    defer gpa.free(root);
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);
    const out_dir = root;
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const a_path = try std.fs.path.join(gpa, &.{ src_dir, "pl-a.md" });
    defer gpa.free(a_path);
    const b_path = try std.fs.path.join(gpa, &.{ src_dir, "pl-b.md" });
    defer gpa.free(b_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = a_path,
        .data =
        \\---
        \\slug: pl-a
        \\description: a
        \\source: docs/a
        \\---
        \\A {{.VendorTitle}}
        ,
    });
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = b_path,
        .data =
        \\---
        \\slug: pl-b
        \\description: b
        \\source: docs/b
        \\---
        \\B {{.VendorTitle}}
        ,
    });
    var res = try renderTree(gpa, .{
        .src_dir = src_dir,
        .out_dir = out_dir,
        .slug_filter = &.{"pl-a"},
    });
    defer res.deinit(gpa);
    try std.testing.expectEqual(@as(usize, 3), res.written_paths.len);
    try std.testing.expectEqual(@as(usize, 1), res.skipped_slugs.len);
    try std.testing.expectEqualStrings("pl-b", res.skipped_slugs[0]);
}

test "checkTree reports content drift and diff" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "skills-render-check" });
    defer gpa.free(root);
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    const src_path = try std.fs.path.join(gpa, &.{ src_dir, "pl-a.md" });
    defer gpa.free(src_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = src_path,
        .data =
        \\---
        \\slug: pl-a
        \\description: a
        \\source: docs/a
        \\---
        \\A {{.VendorTitle}}
        ,
    });
    var rendered = try renderTree(gpa, .{
        .src_dir = src_dir,
        .out_dir = root,
    });
    defer rendered.deinit(gpa);
    const tampered = try std.fs.path.join(gpa, &.{ root, "commands", "claude", "pl-a.md" });
    defer gpa.free(tampered);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = tampered,
        .data = "tampered\n",
    });
    var check = try checkTree(gpa, .{
        .src_dir = src_dir,
        .out_dir = root,
        .emit_diff = true,
    });
    defer check.deinit(gpa);
    try std.testing.expect(!check.inSync());
    try std.testing.expect(check.diffForPath("commands/claude/pl-a.md") != null);
}

test "parseSource rejects unterminated quote" {
    const gpa = std.testing.allocator;
    const raw =
        \\---
        \\slug: "unterminated
        \\description: ok
        \\source: docs/x
        \\---
        \\body
    ;
    try std.testing.expectError(RenderError.ParseFailure, parseSourceBytes(gpa, "skills/src/bad.md", raw));
}

test "parseSource rejects malformed flow list in unsupported key" {
    const gpa = std.testing.allocator;
    const raw =
        \\---
        \\slug: pl-ok
        \\description: ok
        \\source: docs/x
        \\canonical_decisions: [bad
        \\---
        \\body
    ;
    try std.testing.expectError(RenderError.ParseFailure, parseSourceBytes(gpa, "skills/src/bad-flow.md", raw));
}

test "parseSource unescapes double-quoted scalar escapes" {
    const gpa = std.testing.allocator;
    const raw =
        \\---
        \\slug: pl-spec-draft
        \\description: desc
        \\source: docs/x
        \\vendor:
        \\  claude:
        \\    argument_hint: "\"<goal>\""
        \\---
        \\body
    ;
    var src = try parseSourceBytes(gpa, "skills/src/pl-spec-draft.md", raw);
    defer src.deinit(gpa);
    try std.testing.expectEqual(@as(usize, 1), src.vendor.len);
    try std.testing.expectEqualStrings("\"<goal>\"", src.vendor[0].argument_hint);
}

test "render rejects stray template end" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var src = Source{
        .path = try gpa.dupe(u8, "skills/src/x.md"),
        .slug = try gpa.dupe(u8, "x"),
        .description = try gpa.dupe(u8, "desc"),
        .source_link = try gpa.dupe(u8, "docs/x"),
        .model_tier = try gpa.dupe(u8, ""),
        .vendor = &.{},
        .shared_notes = &.{},
        .body = try gpa.dupe(u8, "hi\n{{end}}\n"),
    };
    defer src.deinit(gpa);
    try std.testing.expectError(RenderError.UnknownTemplateToken, render(gpa, src, vendors.get("claude").?));
}

const agent_orchestrator_fixture =
    \\---
    \\name: orchestrator
    \\description: Top-level dispatcher.
    \\tier: large
    \\role: orchestrator
    \\capability: coordinate
    \\---
    \\
    \\# Orchestrator
    \\
    \\Coordination only.
;

const agent_coder_fixture =
    \\---
    \\name: coder
    \\description: Coding agent.
    \\tier: medium
    \\role: coder
    \\capability: write
    \\---
    \\
    \\# Coder
    \\
    \\Implements tasks.
;

const agent_reviewer_fixture =
    \\---
    \\name: reviewer
    \\description: Reviews coder output.
    \\tier: medium
    \\role: reviewer
    \\capability: read-only
    \\---
    \\
    \\# Reviewer
    \\
    \\Reviews only.
;

test "parseAgentSourceFile parses frontmatter and body" {
    const gpa = std.testing.allocator;
    var src = try parseAgentSourceBytes(gpa, "agents/orchestrator.md", agent_orchestrator_fixture);
    defer src.deinit(gpa);
    try std.testing.expectEqualStrings("orchestrator", src.name);
    try std.testing.expectEqualStrings("orchestrator", src.role);
    try std.testing.expectEqualStrings("coordinate", src.capability);
    try std.testing.expectEqualStrings("large", src.tier);
    try std.testing.expect(std.mem.indexOf(u8, src.body, "# Orchestrator") != null);
}

test "parseAgentSourceFile requires name, role, capability, tier" {
    const gpa = std.testing.allocator;
    const missing_cap =
        \\---
        \\name: x
        \\description: d
        \\tier: medium
        \\role: coder
        \\---
        \\body
    ;
    try std.testing.expectError(RenderError.MissingAgentCapability, parseAgentSourceBytes(gpa, "agents/x.md", missing_cap));
}

test "capability maps to tools allowlist per ADR 260" {
    const coord = try capabilityTools("coordinate");
    try std.testing.expectEqual(@as(usize, 5), coord.len);
    // coordinate omits Edit/Write.
    for (coord) |t| {
        try std.testing.expect(!std.mem.eql(u8, t, "Edit"));
        try std.testing.expect(!std.mem.eql(u8, t, "Write"));
    }
    const write = try capabilityTools("write");
    var has_edit = false;
    var has_write = false;
    for (write) |t| {
        if (std.mem.eql(u8, t, "Edit")) has_edit = true;
        if (std.mem.eql(u8, t, "Write")) has_write = true;
    }
    try std.testing.expect(has_edit and has_write);
    const ro = try capabilityTools("read-only");
    try std.testing.expectEqual(@as(usize, 3), ro.len);
    try std.testing.expectError(RenderError.UnknownCapability, capabilityTools("nonsense"));
}

test "capability maps to codex sandbox mode" {
    try std.testing.expectEqualStrings("workspace-write", try capabilitySandboxMode("coordinate"));
    try std.testing.expectEqualStrings("workspace-write", try capabilitySandboxMode("write"));
    try std.testing.expectEqualStrings("read-only", try capabilitySandboxMode("read-only"));
    try std.testing.expectError(RenderError.UnknownCapability, capabilitySandboxMode("nonsense"));
}

test "tier maps to model via existing models table" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var coder = try parseAgentSourceBytes(gpa, "agents/coder.md", agent_coder_fixture);
    defer coder.deinit(gpa);
    const claude = vendors.get("claude").?;
    const model = try resolveAgentModel(gpa, coder, claude);
    defer gpa.free(model);
    try std.testing.expectEqualStrings("claude-sonnet-4-6", model);
}

test "renderAgent emits Claude md-yaml frontmatter with tools and model" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var coder = try parseAgentSourceBytes(gpa, "agents/coder.md", agent_coder_fixture);
    defer coder.deinit(gpa);
    const out = try renderAgent(gpa, coder, vendors.get("claude").?);
    defer gpa.free(out);
    try std.testing.expect(std.mem.startsWith(u8, out, "---\n"));
    try std.testing.expect(std.mem.indexOf(u8, out, "name: coder") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "tools: [Read, Edit, Write, Bash, Grep, Glob]") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "model: claude-sonnet-4-6") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "# Coder") != null);
}

test "renderAgent emits Codex TOML with sandbox_mode and developer_instructions" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var coder = try parseAgentSourceBytes(gpa, "agents/coder.md", agent_coder_fixture);
    defer coder.deinit(gpa);
    const out = try renderAgent(gpa, coder, vendors.get("codex").?);
    defer gpa.free(out);
    try std.testing.expect(std.mem.indexOf(u8, out, "name = \"coder\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "sandbox_mode = \"workspace-write\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "model = \"gpt-5.4\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "model_reasoning_effort = \"medium\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "developer_instructions = \"\"\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "# Coder") != null);
}

test "agent projections carry stable source and vendor digests for all vendors" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var coder = try parseAgentSourceBytes(gpa, "/checkout-a/agents/coder.md", agent_coder_fixture);
    defer coder.deinit(gpa);

    const names = [_][]const u8{ "claude", "codex", "copilot" };
    var projection_values: [names.len][]const u8 = undefined;
    var rendered: [names.len][]u8 = undefined;
    defer {
        for (rendered) |bytes| gpa.free(bytes);
    }
    for (names, 0..) |name, i| {
        rendered[i] = try renderAgent(gpa, coder, vendors.get(name).?);
        const source_value = projectionMetadataValue(rendered[i], SourceDigestKey).?;
        const projection_value = projectionMetadataValue(rendered[i], ProjectionDigestKey).?;
        try std.testing.expectEqual(@as(usize, 64), source_value.len);
        try std.testing.expectEqual(@as(usize, 64), projection_value.len);
        if (i > 0) try std.testing.expectEqualStrings(
            projectionMetadataValue(rendered[0], SourceDigestKey).?,
            source_value,
        );
        projection_values[i] = projection_value;
    }
    try std.testing.expect(!std.mem.eql(u8, projection_values[0], projection_values[1]));
    try std.testing.expect(!std.mem.eql(u8, projection_values[1], projection_values[2]));

    const changed_fixture = agent_coder_fixture ++ "\nChanged authored input.\n";
    var changed_source = try parseAgentSourceBytes(gpa, "/checkout-b/agents/coder.md", changed_fixture);
    defer changed_source.deinit(gpa);
    const changed = try renderAgent(gpa, changed_source, vendors.get("claude").?);
    defer gpa.free(changed);
    try std.testing.expect(!std.mem.eql(
        u8,
        projectionMetadataValue(rendered[0], SourceDigestKey).?,
        projectionMetadataValue(changed, SourceDigestKey).?,
    ));
    try std.testing.expect(!std.mem.eql(
        u8,
        projectionMetadataValue(rendered[0], ProjectionDigestKey).?,
        projectionMetadataValue(changed, ProjectionDigestKey).?,
    ));
}

test "renderAgent Codex TOML developer_instructions escapes triple-quote and backslash" {
    // A body that contains `"""` (e.g. a Python docstring example) and a
    // backslash (e.g. a regex or path) must not prematurely close the
    // TOML multi-line basic string and must survive a round-trip through
    // the TOML escape rules.
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const tricky_fixture =
        \\---
        \\name: tricky
        \\description: Agent with tricky body.
        \\tier: medium
        \\role: coder
        \\capability: write
        \\---
        \\
        \\Use `"""` for Python docstrings.
        \\Also a backslash: C:\Users\foo
    ;
    var src = try parseAgentSourceBytes(gpa, "agents/tricky.md", tricky_fixture);
    defer src.deinit(gpa);
    const out = try renderAgent(gpa, src, vendors.get("codex").?);
    defer gpa.free(out);

    // The emitted TOML must open and close developer_instructions exactly once.
    const open_idx = std.mem.indexOf(u8, out, "developer_instructions = \"\"\"\n");
    try std.testing.expect(open_idx != null);
    // There must be a closing """ that is NOT premature — the body's """ must
    // have been escaped. Verify the raw sequence `"""\n` appears only twice
    // (once to open, once to close) — if the body `"""` leaked unescaped
    // there would be a third occurrence closing mid-body.
    var count: usize = 0;
    var search = out;
    while (std.mem.indexOf(u8, search, "\"\"\"\n")) |pos| {
        count += 1;
        search = search[pos + 4 ..];
    }
    try std.testing.expectEqual(@as(usize, 2), count);

    // The body's backslash must be doubled in the TOML output.
    try std.testing.expect(std.mem.indexOf(u8, out, "C:\\\\Users\\\\foo") != null);
    // The body's `"""` must be escaped (not appear raw in body section).
    // After the opening `"""\n`, the next `"""` must be the closing one —
    // verify no unescaped `"""` exists before the final `"""\n`.
    const body_start = open_idx.? + "developer_instructions = \"\"\"\n".len;
    const closing_idx = std.mem.lastIndexOf(u8, out, "\"\"\"\n").?;
    const body_region = out[body_start..closing_idx];
    try std.testing.expect(std.mem.indexOf(u8, body_region, "\"\"\"") == null);
}

test "renderAgent Codex coordinate role injects weak-enforcement note" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    var orch = try parseAgentSourceBytes(gpa, "agents/orchestrator.md", agent_orchestrator_fixture);
    defer orch.deinit(gpa);
    const out = try renderAgent(gpa, orch, vendors.get("codex").?);
    defer gpa.free(out);
    // coordinate -> workspace-write per the caveat (read-only would block DB writes).
    try std.testing.expect(std.mem.indexOf(u8, out, "sandbox_mode = \"workspace-write\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "Codex enforcement caveat") != null);
    try std.testing.expect(std.mem.indexOf(u8, out, "model_reasoning_effort = \"high\"") != null);
}

test "orchestrator renders with NO Edit/Write; coder renders WITH them at sonnet (Claude)" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const claude = vendors.get("claude").?;

    var orch = try parseAgentSourceBytes(gpa, "agents/orchestrator.md", agent_orchestrator_fixture);
    defer orch.deinit(gpa);
    const orch_out = try renderAgent(gpa, orch, claude);
    defer gpa.free(orch_out);
    try std.testing.expect(std.mem.indexOf(u8, orch_out, "tools: [Bash, Agent, Read, Grep, Glob]") != null);
    try std.testing.expect(std.mem.indexOf(u8, orch_out, "Edit") == null);
    try std.testing.expect(std.mem.indexOf(u8, orch_out, "Write") == null);
    try std.testing.expect(std.mem.indexOf(u8, orch_out, "model: claude-opus-4-8") != null);

    var coder = try parseAgentSourceBytes(gpa, "agents/coder.md", agent_coder_fixture);
    defer coder.deinit(gpa);
    const coder_out = try renderAgent(gpa, coder, claude);
    defer gpa.free(coder_out);
    try std.testing.expect(std.mem.indexOf(u8, coder_out, "Edit") != null);
    try std.testing.expect(std.mem.indexOf(u8, coder_out, "Write") != null);
    try std.testing.expect(std.mem.indexOf(u8, coder_out, "model: claude-sonnet-4-6") != null);
}

test "Copilot agent files use .agent.md extension" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const copilot = vendors.get("copilot").?;
    const name = try agentOutputName(gpa, "orchestrator", copilot);
    defer gpa.free(name);
    try std.testing.expectEqualStrings("orchestrator.agent.md", name);

    const codex_name = try agentOutputName(gpa, "orchestrator", vendors.get("codex").?);
    defer gpa.free(codex_name);
    try std.testing.expectEqualStrings("orchestrator.toml", codex_name);

    const claude_name = try agentOutputName(gpa, "orchestrator", vendors.get("claude").?);
    defer gpa.free(claude_name);
    try std.testing.expectEqualStrings("orchestrator.md", claude_name);
}

test "isAgentRoleFile skips non-role docs" {
    try std.testing.expect(isAgentRoleFile("orchestrator.md"));
    try std.testing.expect(!isAgentRoleFile("methodology.md"));
    try std.testing.expect(!isAgentRoleFile("models.md"));
    try std.testing.expect(!isAgentRoleFile("doctrine.md"));
    try std.testing.expect(!isAgentRoleFile("README.txt"));
}

test "renderTree renders agents per vendor and leaves models.md patch intact" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "agents-render" });
    defer gpa.free(root);
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);
    const agents_src = try std.fs.path.join(gpa, &.{ root, "agents" });
    defer gpa.free(agents_src);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, agents_src);

    // one skill source so renderTree has a skill leaf too
    const skill_path = try std.fs.path.join(gpa, &.{ src_dir, "pl-a.md" });
    defer gpa.free(skill_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = skill_path,
        .data =
        \\---
        \\slug: pl-a
        \\description: a
        \\source: docs/a
        \\---
        \\A {{.VendorTitle}}
        ,
    });
    // agent role specs + a non-role doc that must be skipped
    const orch_path = try std.fs.path.join(gpa, &.{ agents_src, "orchestrator.md" });
    defer gpa.free(orch_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = orch_path, .data = agent_orchestrator_fixture });
    const coder_path = try std.fs.path.join(gpa, &.{ agents_src, "coder.md" });
    defer gpa.free(coder_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = coder_path, .data = agent_coder_fixture });
    const methodology_path = try std.fs.path.join(gpa, &.{ agents_src, "methodology.md" });
    defer gpa.free(methodology_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = methodology_path, .data = "# Methodology\n\nNo frontmatter.\n" });
    // models.md with a tier table for the patch path
    const models_path = try std.fs.path.join(gpa, &.{ agents_src, "models.md" });
    defer gpa.free(models_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = models_path,
        .data =
        \\# Models
        \\
        \\## Tier Table
        \\
        \\| Tier | Claude | Codex | Copilot |
        \\|------|--------|-------|---------|
        \\| medium | stale | stale | stale |
        \\| large | stale | stale | stale |
        \\
        \\## Agent Assignments
        ,
    });

    var res = try renderTree(gpa, .{ .src_dir = src_dir, .out_dir = root });
    defer res.deinit(gpa);

    // agent files rendered per vendor
    const claude_orch = try std.fs.path.join(gpa, &.{ root, "agents", "claude", "orchestrator.md" });
    defer gpa.free(claude_orch);
    const claude_orch_bytes = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, claude_orch, gpa, std.Io.Limit.limited(1 << 20));
    defer gpa.free(claude_orch_bytes);
    try std.testing.expect(std.mem.indexOf(u8, claude_orch_bytes, "tools: [Bash, Agent, Read, Grep, Glob]") != null);

    const codex_coder = try std.fs.path.join(gpa, &.{ root, "agents", "codex", "coder.toml" });
    defer gpa.free(codex_coder);
    const codex_coder_bytes = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, codex_coder, gpa, std.Io.Limit.limited(1 << 20));
    defer gpa.free(codex_coder_bytes);
    try std.testing.expect(std.mem.indexOf(u8, codex_coder_bytes, "sandbox_mode = \"workspace-write\"") != null);

    const copilot_orch = try std.fs.path.join(gpa, &.{ root, "agents", "copilot", "orchestrator.agent.md" });
    defer gpa.free(copilot_orch);
    const copilot_orch_bytes = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, copilot_orch, gpa, std.Io.Limit.limited(1 << 20));
    defer gpa.free(copilot_orch_bytes);
    try std.testing.expect(std.mem.indexOf(u8, copilot_orch_bytes, "tools: [Bash, Agent, Read, Grep, Glob]") != null);

    // methodology.md (non-role) was NOT rendered as an agent
    const claude_methodology = try std.fs.path.join(gpa, &.{ root, "agents", "claude", "methodology.md" });
    defer gpa.free(claude_methodology);
    try std.testing.expectError(error.FileNotFound, std.Io.Dir.cwd().readFileAlloc(std.testing.io, claude_methodology, gpa, std.Io.Limit.limited(1 << 20)));

    // models.md tier table patched (stale replaced)
    const models_after = try std.Io.Dir.cwd().readFileAlloc(std.testing.io, models_path, gpa, std.Io.Limit.limited(1 << 20));
    defer gpa.free(models_after);
    try std.testing.expect(std.mem.indexOf(u8, models_after, "stale") == null);
    try std.testing.expect(std.mem.indexOf(u8, models_after, "claude-sonnet-4-6") != null);

    // re-running checkTree should report in-sync (idempotent render + agent check)
    var check = try checkTree(gpa, .{ .src_dir = src_dir, .out_dir = root });
    defer check.deinit(gpa);
    if (!check.inSync()) {
        for (check.drifted) |d| std.debug.print("unexpected drift: [{s}] {s}\n", .{ d.reason.text(), d.path });
    }
    try std.testing.expect(check.inSync());
}

test "checkTree detects agent content drift and orphan" {
    const gpa = std.testing.allocator;
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();
    const root = try std.fs.path.join(gpa, &.{ ".zig-cache/tmp", &tmp.sub_path, "agents-check" });
    defer gpa.free(root);
    const src_dir = try std.fs.path.join(gpa, &.{ root, "skills", "src" });
    defer gpa.free(src_dir);
    const agents_src = try std.fs.path.join(gpa, &.{ root, "agents" });
    defer gpa.free(agents_src);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, src_dir);
    try std.Io.Dir.cwd().createDirPath(std.testing.io, agents_src);
    const orch_path = try std.fs.path.join(gpa, &.{ agents_src, "orchestrator.md" });
    defer gpa.free(orch_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = orch_path, .data = agent_orchestrator_fixture });
    const models_path = try std.fs.path.join(gpa, &.{ agents_src, "models.md" });
    defer gpa.free(models_path);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{
        .sub_path = models_path,
        .data =
        \\# Models
        \\
        \\## Tier Table
        \\
        \\| Tier | Claude | Codex | Copilot |
        \\|------|--------|-------|---------|
        \\| medium | a | b | c |
        \\| large | a | b | c |
        \\
        \\## End
        ,
    });

    var rendered = try renderTree(gpa, .{ .src_dir = src_dir, .out_dir = root });
    defer rendered.deinit(gpa);

    // tamper the Claude agent file
    const tampered = try std.fs.path.join(gpa, &.{ root, "agents", "claude", "orchestrator.md" });
    defer gpa.free(tampered);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = tampered, .data = "tampered\n" });
    // add an orphan in the codex agents dir
    const orphan = try std.fs.path.join(gpa, &.{ root, "agents", "codex", "ghost.toml" });
    defer gpa.free(orphan);
    try std.Io.Dir.cwd().writeFile(std.testing.io, .{ .sub_path = orphan, .data = "name = \"ghost\"\n" });

    var check = try checkTree(gpa, .{ .src_dir = src_dir, .out_dir = root, .emit_diff = true });
    defer check.deinit(gpa);
    try std.testing.expect(!check.inSync());
    var saw_content = false;
    var saw_orphan = false;
    for (check.drifted) |d| {
        if (d.reason == .content and std.mem.indexOf(u8, d.path, "agents/claude/orchestrator.md") != null) saw_content = true;
        if (d.reason == .orphan and std.mem.indexOf(u8, d.path, "agents/codex/ghost.toml") != null) saw_orphan = true;
    }
    try std.testing.expect(saw_content);
    try std.testing.expect(saw_orphan);
}

test "renderModelsDoc preserves preferred tier order and separator widths" {
    const gpa = std.testing.allocator;
    var vendors = try loadVendors(gpa);
    defer vendors.deinit(gpa);
    const seed =
        \\# Models
        \\
        \\## Tier Table
        \\
        \\| Tier | Claude | Codex | Copilot |
        \\|------|--------|-------|---------|
        \\| medium | stale | stale | stale |
        \\| large | stale | stale | stale |
        \\
        \\## Agent Assignments
    ;
    const got = try renderModelsDoc(gpa, seed, vendors);
    defer gpa.free(got);
    const medium_idx = std.mem.indexOf(u8, got, "| medium |");
    const large_idx = std.mem.indexOf(u8, got, "| large |");
    try std.testing.expect(medium_idx != null and large_idx != null and medium_idx.? < large_idx.?);
    try std.testing.expect(std.mem.indexOf(u8, got, "|------|--------|-----|-------|") == null);
    try std.testing.expect(std.mem.indexOf(u8, got, "| ------ | ------ | ----- | ------- |") != null);
}
