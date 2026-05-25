//! handlers/ext/adapter_factory — build extsync.Transport-backed Jira/GitHub
//! adapters from an ExternalSystem row + the process environ.
//!
//! Each registered system carries `auth_method` + `auth_ref`:
//!   - "token-env"    → look up auth_ref in environ, use as bearer token
//!   - "gh-cli"       → subprocess-exec `gh auth token` and use stdout
//!                      as a bearer token. Token is captured per request
//!                      (the Handle is per-handler-invocation, so caching
//!                      is per-process and bounded by the handler's lifetime).
//!   - "oauth-stored" → not yet supported (matches Go: deferred to M5c).
//!                      Returns a clear user-facing error.
//!
//! The Transport is implemented on top of std.http.Client (one Client per
//! handler invocation, lives as long as the adapter).

const std = @import("std");
const extsync = @import("engine").extsync;
const engine_external = @import("engine").external;

/// Owned adapter handle. Holds the heap-allocated http client + a copy of the
/// auth token so the adapter's []const u8 references stay valid until deinit.
pub const Handle = struct {
    allocator: std.mem.Allocator,
    client: *std.http.Client,
    token: []u8,
    base_url_owned: []u8,
    kind: enum { jira, github },
    jira_adapter: ?extsync.jira.JiraAdapter = null,
    github_adapter: ?extsync.github.GithubAdapter = null,

    pub fn deinit(self: *Handle) void {
        self.client.deinit();
        self.allocator.destroy(self.client);
        self.allocator.free(self.token);
        self.allocator.free(self.base_url_owned);
    }

    /// Borrow the underlying transport. Lives as long as `self`.
    pub fn transport(self: *Handle) extsync.common.Transport {
        return .{ .ctx = self.client, .sendFn = httpSend };
    }
};

pub const Error = error{
    UnsupportedAuthMethod,
    TokenEnvVarMissing,
    GhCliNotFound,
    GhCliFailed,
    GhCliEmptyToken,
    UnsupportedSystemKind,
} || std.mem.Allocator.Error;

/// Build an adapter for the given system row. Caller owns the returned Handle
/// and must call `deinit` when finished.
///
/// The returned handle exposes either `jira_adapter` or `github_adapter`
/// (the other is null). Use the matching field for dispatch.
pub fn build(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: *const std.process.Environ,
    sys: engine_external.system.ExternalSystem,
) Error!*Handle {
    const cred = try resolveCredential(allocator, io, environ, sys);
    errdefer allocator.free(cred);

    const base_url_src: []const u8 = sys.base_url orelse "";
    const base_url_owned = try allocator.dupe(u8, base_url_src);
    errdefer allocator.free(base_url_owned);

    const client = try allocator.create(std.http.Client);
    errdefer allocator.destroy(client);
    client.* = .{ .allocator = allocator, .io = io };

    var handle = try allocator.create(Handle);
    errdefer allocator.destroy(handle);
    handle.* = .{
        .allocator = allocator,
        .client = client,
        .token = cred,
        .base_url_owned = base_url_owned,
        .kind = undefined,
    };

    const auth_cred: extsync.common.AuthCredential = .{ .kind = .bearer, .token = cred };
    switch (sys.kind) {
        .jira => {
            handle.kind = .jira;
            handle.jira_adapter = extsync.jira.JiraAdapter.init(base_url_owned, auth_cred, handle.transport());
        },
        .@"github-issues" => {
            handle.kind = .github;
            handle.github_adapter = extsync.github.GithubAdapter.init(base_url_owned, auth_cred, handle.transport());
        },
        else => return Error.UnsupportedSystemKind,
    }
    return handle;
}

fn resolveCredential(
    allocator: std.mem.Allocator,
    io: std.Io,
    environ: *const std.process.Environ,
    sys: engine_external.system.ExternalSystem,
) Error![]u8 {
    switch (sys.auth_method) {
        .@"token-env" => {
            const env_name = sys.auth_ref;
            const v = environ.getPosix(env_name) orelse return Error.TokenEnvVarMissing;
            return try allocator.dupe(u8, v);
        },
        .@"gh-cli" => return try resolveGhCli(allocator, io),
        // Match Go: oauth-stored is documented and accepted but the
        // resolver still returns a clear "not yet supported" error. The
        // Zig and Go binaries agree: same auth_method enumeration, same
        // deferred-feature error.
        .@"oauth-stored" => return Error.UnsupportedAuthMethod,
    }
}

/// resolveGhCli shells out to `gh auth token` and returns the trimmed
/// stdout as a bearer token. Mirrors Go's `resolveGHCLI`. Returns
/// `error.GhCliNotFound` when the binary cannot be spawned,
/// `error.GhCliFailed` when it exits non-zero, and `error.GhCliEmptyToken`
/// when stdout is empty after trimming.
fn resolveGhCli(allocator: std.mem.Allocator, io: std.Io) Error![]u8 {
    const argv = [_][]const u8{ "gh", "auth", "token" };
    const result = std.process.run(allocator, io, .{ .argv = &argv }) catch
        return Error.GhCliNotFound;
    defer allocator.free(result.stderr);
    defer allocator.free(result.stdout);

    switch (result.term) {
        .exited => |code| if (code != 0) return Error.GhCliFailed,
        else => return Error.GhCliFailed,
    }

    const trimmed = std.mem.trim(u8, result.stdout, " \t\r\n");
    if (trimmed.len == 0) return Error.GhCliEmptyToken;
    return try allocator.dupe(u8, trimmed);
}

/// Transport.send implementation backed by std.http.Client.fetch.
fn httpSend(
    ctx: *anyopaque,
    allocator: std.mem.Allocator,
    req: extsync.common.Request,
) anyerror!extsync.common.Response {
    const client: *std.http.Client = @ptrCast(@alignCast(ctx));

    var body_buf: std.Io.Writer.Allocating = .init(allocator);
    defer body_buf.deinit();

    var extra_headers: std.ArrayList(std.http.Header) = .empty;
    defer extra_headers.deinit(allocator);
    for (req.headers) |h| {
        try extra_headers.append(allocator, .{ .name = h.name, .value = h.value });
    }

    const method: std.http.Method = switch (req.method) {
        .get => .GET,
        .post => .POST,
        .put => .PUT,
        .patch => .PATCH,
    };

    const result = client.fetch(.{
        .location = .{ .url = req.url },
        .method = method,
        .payload = req.body,
        .extra_headers = extra_headers.items,
        .response_writer = &body_buf.writer,
    }) catch return error.HttpFetchFailed;

    return .{
        .status = @intFromEnum(result.status),
        .body = try allocator.dupe(u8, body_buf.written()),
    };
}
