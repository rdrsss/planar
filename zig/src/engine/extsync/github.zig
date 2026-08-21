//! engine/extsync/github - GitHub Issues operational-plane adapter.

const std = @import("std");
const extsync = @import("common.zig");

pub const api_base_default = "https://api.github.com";

pub const Error = error{
    InvalidExternalId,
    NotFound,
    UnexpectedStatus,
    TransportFailed,
    ParseFailed,
    InvalidAuth,
    WriteFailed,
    /// GitHub GraphQL returned a 200 with a non-empty top-level `errors` array.
    GraphqlError,
} || std.mem.Allocator.Error;

pub const GithubAdapter = struct {
    base_url: []const u8,
    cred: extsync.AuthCredential,
    transport: extsync.Transport,

    pub fn init(base_url: []const u8, cred: extsync.AuthCredential, transport: extsync.Transport) GithubAdapter {
        const effective = if (base_url.len == 0) api_base_default else base_url;
        return .{
            .base_url = trimTrailingSlash(effective),
            .cred = cred,
            .transport = transport,
        };
    }

    pub fn validate(_: *const GithubAdapter, external_id: []const u8) Error!void {
        _ = parseExternalID(external_id) catch return Error.InvalidExternalId;
    }

    pub fn pull(self: *const GithubAdapter, allocator: std.mem.Allocator, external_id: []const u8) Error!extsync.RemoteState {
        const parsed = parseExternalID(external_id) catch return Error.InvalidExternalId;
        const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/{s}/issues/{d}", .{
            self.base_url,
            parsed.owner,
            parsed.repo,
            parsed.number,
        });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .get,
            .url = url,
            .headers = &headers,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);

        if (response.status == 404) return Error.NotFound;
        if (response.status != 200) return Error.UnexpectedStatus;
        return try parseIssue(allocator, external_id, response.body);
    }

    pub fn push(self: *const GithubAdapter, allocator: std.mem.Allocator, external_id: []const u8, fields: extsync.FieldChangeSet) Error!extsync.UpdateOutcome {
        const parsed = parseExternalID(external_id) catch return Error.InvalidExternalId;
        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const writer = &out.writer;
        try writer.writeAll("{");
        var has_field = false;

        var applied: std.ArrayList([]const u8) = .empty;
        defer applied.deinit(allocator);

        if (fields.title) |title| {
            has_field = true;
            try writer.writeAll("\"title\":");
            try writeJSONString(writer, title);
            try applied.append(allocator, "title");
        }
        if (fields.status) |status| {
            const mapping = stateForStatus(status);
            if (has_field) try writer.writeAll(",");
            has_field = true;
            try writer.writeAll("\"state\":");
            try writeJSONString(writer, mapping.state);
            if (mapping.labels) |labels| {
                try writer.writeAll(",\"labels\":[");
                for (labels, 0..) |label, i| {
                    if (i != 0) try writer.writeAll(",");
                    try writeJSONString(writer, label);
                }
                try writer.writeAll("]");
            }
            try applied.append(allocator, "status");
        }
        if (fields.assignee) |assignee| {
            if (has_field) try writer.writeAll(",");
            has_field = true;
            try writer.writeAll("\"assignees\":[");
            try writeJSONString(writer, assignee);
            try writer.writeAll("]");
            try applied.append(allocator, "assignee");
        }
        try writer.writeAll("}");

        if (!has_field) return .{};

        const payload = try allocator.dupe(u8, out.written());
        defer allocator.free(payload);
        const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/{s}/issues/{d}", .{
            self.base_url,
            parsed.owner,
            parsed.repo,
            parsed.number,
        });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .patch,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status != 200) return Error.UnexpectedStatus;
        return .{ .fields_applied = try applied.toOwnedSlice(allocator) };
    }

    /// CreatedIssue is the (number, node_id) pair the REST `POST /repos/{o}/{r}/issues`
    /// endpoint returns. Caller owns `node_id`.
    pub const CreatedIssue = struct {
        number: i64,
        node_id: []u8,
    };

    /// createIssue creates a regular GitHub issue and returns both the issue
    /// number and the `node_id` (needed for ProjectsV2 addProjectV2ItemById).
    ///
    /// Mirrors Go `internal/adapter/github_graphql.go::CreateIssueWithNodeID`.
    /// REST: `POST /repos/{owner}/{repo}/issues` with `{title, body, labels?}`.
    /// Returns `UnexpectedStatus` if the server returns anything but 201.
    pub fn createIssue(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) Error!CreatedIssue {
        const payload = try buildCreateIssueBody(allocator, title, body, labels);
        defer allocator.free(payload);

        const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/{s}/issues", .{
            self.base_url, owner, repo,
        });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .post,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status != 201) return Error.UnexpectedStatus;

        return try parseCreatedIssue(allocator, response.body);
    }

    /// linkSubIssue calls the GitHub REST sub-issue endpoint to parent
    /// `child_number` under `parent_number` in the same repo.
    ///
    /// Mirrors Go `internal/adapter/github_graphql.go::linkSubIssue`.
    /// REST: `POST /repos/{owner}/{repo}/issues/{parent_number}/sub_issues`
    /// with body `{"sub_issue_id": <child_number>}`.
    /// A 404 response is mapped to `NotFound` so the caller can detect
    /// "sub-issue endpoint unavailable on this account".
    pub fn linkSubIssue(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
        parent_number: i64,
        child_number: i64,
    ) Error!void {
        const payload = try std.fmt.allocPrint(
            allocator,
            "{{\"sub_issue_id\":{d}}}",
            .{child_number},
        );
        defer allocator.free(payload);

        const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/{s}/issues/{d}/sub_issues", .{
            self.base_url, owner, repo, parent_number,
        });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .post,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status == 404) return Error.NotFound;
        if (response.status != 200 and response.status != 201) return Error.UnexpectedStatus;
    }

    /// createSubIssue creates an issue and immediately parents it under
    /// `parent_number` via the sub-issue REST endpoint.
    ///
    /// Mirrors Go `internal/adapter/github_graphql.go::CreateSubIssue`.
    /// On link failure (404 from the sub-issue endpoint, etc.), the created
    /// issue is still returned alongside the error so the caller can decide
    /// whether to keep the orphaned issue or fall back to a different strategy.
    pub fn createSubIssue(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        parent_number: i64,
        owner: []const u8,
        repo: []const u8,
        title: []const u8,
        body: []const u8,
        labels: []const []const u8,
    ) Error!CreatedIssue {
        const created = try self.createIssue(allocator, owner, repo, title, body, labels);
        errdefer allocator.free(created.node_id);

        self.linkSubIssue(allocator, owner, repo, parent_number, created.number) catch |e| {
            // Surface the link failure but free the node_id since callers
            // can't usefully consume the half-formed result here.
            allocator.free(created.node_id);
            return e;
        };
        return created;
    }

    /// linkSubIssueProbe probes whether the sub-issues REST endpoint is
    /// available for `owner/repo` without creating any real relation.
    ///
    /// Mirrors Go `internal/adapter/github_graphql.go::LinkSubIssueProbe`.
    /// Strategy: POST to `issues/0/sub_issues` with `sub_issue_id:0`. A 404
    /// returns `NotFound` (endpoint not enabled for the account); any other
    /// status (404 for the bogus issue id, 422, etc.) returns success.
    pub fn linkSubIssueProbe(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        owner: []const u8,
        repo: []const u8,
    ) Error!void {
        const url = try std.fmt.allocPrint(allocator, "{s}/repos/{s}/{s}/issues/0/sub_issues", .{
            self.base_url, owner, repo,
        });
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .post,
            .url = url,
            .headers = &headers,
            .body = "{\"sub_issue_id\":0}",
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status == 404) return Error.NotFound;
        // Every other status (including 422 and 422-with-issue-error) means
        // the endpoint exists.
    }

    /// postComment posts a comment on an issue identified by `owner/repo#N`.
    ///
    /// Mirrors Go `internal/adapter/github.go::Comment` for the REST issue path.
    /// REST: `POST /repos/{owner}/{repo}/issues/{number}/comments`.
    pub fn postComment(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        external_id: []const u8,
        body: []const u8,
    ) Error!void {
        const parsed = parseExternalID(external_id) catch return Error.InvalidExternalId;
        const url = try std.fmt.allocPrint(
            allocator,
            "{s}/repos/{s}/{s}/issues/{d}/comments",
            .{ self.base_url, parsed.owner, parsed.repo, parsed.number },
        );
        defer allocator.free(url);

        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const w = &out.writer;
        try w.writeAll("{\"body\":");
        try writeJSONString(w, body);
        try w.writeAll("}");
        const payload = try allocator.dupe(u8, out.written());
        defer allocator.free(payload);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/vnd.github+json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .post,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);
        if (response.status != 201 and response.status != 200) return Error.UnexpectedStatus;
    }

    /// ProjectField describes a GitHub Projects v2 custom field discovered via
    /// `getProjectV2Fields`. Caller owns `id`, `name`, and `data_type`.
    ///
    /// Mirrors Go `internal/adapter/github_graphql.go::ProjectField`.
    pub const ProjectField = struct {
        id: []u8,
        name: []u8,
        data_type: []u8,

        pub fn deinit(self: ProjectField, allocator: std.mem.Allocator) void {
            allocator.free(self.id);
            allocator.free(self.name);
            allocator.free(self.data_type);
        }
    };

    /// CreatedProject is the (node_id, url) pair returned by createProjectV2.
    /// Caller owns both slices.
    pub const CreatedProject = struct {
        node_id: []u8,
        url: []u8,

        pub fn deinit(self: CreatedProject, allocator: std.mem.Allocator) void {
            allocator.free(self.node_id);
            allocator.free(self.url);
        }
    };

    /// AuthenticatedOwner is the GraphQL viewer envelope: the user's own node id
    /// plus the list of org node ids the token can see. Caller owns every
    /// allocated slice (the user_node_id string and each org id).
    pub const AuthenticatedOwner = struct {
        user_node_id: []u8,
        org_ids: [][]u8,

        pub fn deinit(self: AuthenticatedOwner, allocator: std.mem.Allocator) void {
            allocator.free(self.user_node_id);
            for (self.org_ids) |id| allocator.free(id);
            allocator.free(self.org_ids);
        }
    };

    /// graphqlDo executes a single GraphQL request (POST to `{base_url}/graphql`)
    /// with a `{"query": ..., "variables": ...}` body. Returns the raw response
    /// body (the `data` envelope) so callers can decode strongly-typed shapes.
    ///
    /// On HTTP non-200, returns `UnexpectedStatus`. When the response carries a
    /// non-empty `errors` array, returns `GraphqlError`. Mirrors Go
    /// `graphqlDo` in `internal/adapter/github_graphql.go`.
    ///
    /// `variables_json` may be null (mutation/query with no variables) or a
    /// pre-rendered JSON object literal. The caller is responsible for
    /// JSON-escaping operator-controlled strings via the `writeGraphqlVar`
    /// helper or `writeJSONString` directly.
    fn graphqlDo(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        query: []const u8,
        variables_json: ?[]const u8,
    ) Error![]u8 {
        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const w = &out.writer;
        try w.writeAll("{\"query\":");
        try writeJSONString(w, query);
        if (variables_json) |vj| {
            try w.writeAll(",\"variables\":");
            try w.writeAll(vj);
        }
        try w.writeAll("}");
        const payload = try allocator.dupe(u8, out.written());
        defer allocator.free(payload);

        const url = try std.fmt.allocPrint(allocator, "{s}/graphql", .{self.base_url});
        defer allocator.free(url);

        const auth = try authHeader(allocator, self.cred);
        defer allocator.free(auth);
        const headers = [_]extsync.Header{
            .{ .name = "Content-Type", .value = "application/json" },
            .{ .name = "Accept", .value = "application/json" },
            .{ .name = "Authorization", .value = auth },
        };
        const response = self.transport.send(allocator, .{
            .method = .post,
            .url = url,
            .headers = &headers,
            .body = payload,
        }) catch return Error.TransportFailed;
        defer response.deinit(allocator);

        if (response.status == 404) return Error.NotFound;
        if (response.status != 200) return Error.UnexpectedStatus;

        // GitHub GraphQL returns 200 even on logical failure; check `errors`
        // array in the envelope. We return the raw body for the caller to parse
        // `data`; here we just gate on the presence of `errors`.
        var parsed = std.json.parseFromSlice(std.json.Value, allocator, response.body, .{}) catch return Error.ParseFailed;
        defer parsed.deinit();
        if (parsed.value != .object) return Error.ParseFailed;
        if (parsed.value.object.get("errors")) |errs_v| {
            if (errs_v == .array and errs_v.array.items.len > 0) {
                return Error.GraphqlError;
            }
        }
        return try allocator.dupe(u8, response.body);
    }

    /// getAuthenticatedOwner returns the viewer's user node_id plus every org
    /// node_id the token can see (up to 20). Used by projects-v2 to decide
    /// whether to create the Project at the org or user level.
    ///
    /// Mirrors Go `GetAuthenticatedOwner` in `github_graphql.go`. On any
    /// transport or GraphQL error, propagates the error from graphqlDo.
    pub fn getAuthenticatedOwner(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
    ) Error!AuthenticatedOwner {
        const q =
            \\query { viewer { id organizations(first: 20) { nodes { id } } } }
        ;
        const body = try self.graphqlDo(allocator, q, null);
        defer allocator.free(body);

        var parsed = std.json.parseFromSlice(std.json.Value, allocator, body, .{}) catch return Error.ParseFailed;
        defer parsed.deinit();
        if (parsed.value != .object) return Error.ParseFailed;
        const data_v = parsed.value.object.get("data") orelse return Error.ParseFailed;
        if (data_v != .object) return Error.ParseFailed;
        const viewer_v = data_v.object.get("viewer") orelse return Error.ParseFailed;
        if (viewer_v != .object) return Error.ParseFailed;

        const user_id_text = getObjectString(viewer_v.object, "id") orelse "";
        const user_node_id = try allocator.dupe(u8, user_id_text);
        errdefer allocator.free(user_node_id);

        var org_ids: std.ArrayList([]u8) = .empty;
        errdefer {
            for (org_ids.items) |s| allocator.free(s);
            org_ids.deinit(allocator);
        }
        if (viewer_v.object.get("organizations")) |orgs_v| {
            if (orgs_v == .object) {
                if (orgs_v.object.get("nodes")) |nodes_v| {
                    if (nodes_v == .array) {
                        for (nodes_v.array.items) |node_v| {
                            if (node_v != .object) continue;
                            const id_text = getObjectString(node_v.object, "id") orelse continue;
                            if (id_text.len == 0) continue;
                            try org_ids.append(allocator, try allocator.dupe(u8, id_text));
                        }
                    }
                }
            }
        }
        return .{
            .user_node_id = user_node_id,
            .org_ids = try org_ids.toOwnedSlice(allocator),
        };
    }

    /// createProjectV2 creates a Projects v2 project owned by `owner_node_id`
    /// with the given `title`. Returns the project's node_id + URL.
    ///
    /// Mirrors Go `CreateProjectV2` in `github_graphql.go`. The GraphQL
    /// mutation is `createProjectV2(input: { ownerId, title })`.
    pub fn createProjectV2(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        owner_node_id: []const u8,
        title: []const u8,
    ) Error!CreatedProject {
        const mutation =
            \\mutation CreateProject($ownerId: ID!, $title: String!) {
            \\  createProjectV2(input: { ownerId: $ownerId, title: $title }) {
            \\    projectV2 { id url }
            \\  }
            \\}
        ;
        var vars_buf: std.Io.Writer.Allocating = .init(allocator);
        defer vars_buf.deinit();
        const vw = &vars_buf.writer;
        try vw.writeAll("{\"ownerId\":");
        try writeJSONString(vw, owner_node_id);
        try vw.writeAll(",\"title\":");
        try writeJSONString(vw, title);
        try vw.writeAll("}");
        const vars_json = try allocator.dupe(u8, vars_buf.written());
        defer allocator.free(vars_json);

        const body = try self.graphqlDo(allocator, mutation, vars_json);
        defer allocator.free(body);

        var parsed = std.json.parseFromSlice(std.json.Value, allocator, body, .{}) catch return Error.ParseFailed;
        defer parsed.deinit();
        const data_v = (parsed.value.object.get("data") orelse return Error.ParseFailed);
        if (data_v != .object) return Error.ParseFailed;
        const create_v = data_v.object.get("createProjectV2") orelse return Error.ParseFailed;
        if (create_v != .object) return Error.ParseFailed;
        const proj_v = create_v.object.get("projectV2") orelse return Error.ParseFailed;
        if (proj_v != .object) return Error.ParseFailed;

        const id_text = getObjectString(proj_v.object, "id") orelse return Error.ParseFailed;
        const url_text = getObjectString(proj_v.object, "url") orelse "";
        const id_owned = try allocator.dupe(u8, id_text);
        errdefer allocator.free(id_owned);
        const url_owned = try allocator.dupe(u8, url_text);
        return .{ .node_id = id_owned, .url = url_owned };
    }

    /// addProjectV2Item adds a content item (issue/PR node_id) to the Project.
    /// Returns the item id assigned by the Project.
    ///
    /// Mirrors Go `AddProjectV2Item`. Mutation: `addProjectV2ItemById`.
    pub fn addProjectV2Item(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        content_node_id: []const u8,
    ) Error![]u8 {
        const mutation =
            \\mutation AddItem($projectId: ID!, $contentId: ID!) {
            \\  addProjectV2ItemById(input: { projectId: $projectId, contentId: $contentId }) {
            \\    item { id }
            \\  }
            \\}
        ;
        var vars_buf: std.Io.Writer.Allocating = .init(allocator);
        defer vars_buf.deinit();
        const vw = &vars_buf.writer;
        try vw.writeAll("{\"projectId\":");
        try writeJSONString(vw, project_node_id);
        try vw.writeAll(",\"contentId\":");
        try writeJSONString(vw, content_node_id);
        try vw.writeAll("}");
        const vars_json = try allocator.dupe(u8, vars_buf.written());
        defer allocator.free(vars_json);

        const body = try self.graphqlDo(allocator, mutation, vars_json);
        defer allocator.free(body);

        var parsed = std.json.parseFromSlice(std.json.Value, allocator, body, .{}) catch return Error.ParseFailed;
        defer parsed.deinit();
        const data_v = parsed.value.object.get("data") orelse return Error.ParseFailed;
        if (data_v != .object) return Error.ParseFailed;
        const add_v = data_v.object.get("addProjectV2ItemById") orelse return Error.ParseFailed;
        if (add_v != .object) return Error.ParseFailed;
        const item_v = add_v.object.get("item") orelse return Error.ParseFailed;
        if (item_v != .object) return Error.ParseFailed;
        const id_text = getObjectString(item_v.object, "id") orelse return Error.ParseFailed;
        return try allocator.dupe(u8, id_text);
    }

    /// setProjectV2ItemFieldValue sets a TEXT-typed field value on a Project
    /// item. Non-text fields (single-select, number) are intentionally NOT
    /// supported in this phase — matches Go's behaviour, which only sets the
    /// `Parent` text field today.
    ///
    /// Mirrors Go `SetProjectV2ItemFieldValue`. Mutation:
    /// `updateProjectV2ItemFieldValue` with a `{"text": "..."}` value input.
    pub fn setProjectV2ItemFieldValue(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
        item_id: []const u8,
        field_id: []const u8,
        text_value: []const u8,
    ) Error!void {
        const mutation =
            \\mutation SetField($projectId: ID!, $itemId: ID!, $fieldId: ID!, $value: ProjectV2FieldValue!) {
            \\  updateProjectV2ItemFieldValue(input: {
            \\    projectId: $projectId, itemId: $itemId, fieldId: $fieldId, value: $value
            \\  }) { projectV2Item { id } }
            \\}
        ;
        var vars_buf: std.Io.Writer.Allocating = .init(allocator);
        defer vars_buf.deinit();
        const vw = &vars_buf.writer;
        try vw.writeAll("{\"projectId\":");
        try writeJSONString(vw, project_node_id);
        try vw.writeAll(",\"itemId\":");
        try writeJSONString(vw, item_id);
        try vw.writeAll(",\"fieldId\":");
        try writeJSONString(vw, field_id);
        try vw.writeAll(",\"value\":{\"text\":");
        try writeJSONString(vw, text_value);
        try vw.writeAll("}}");
        const vars_json = try allocator.dupe(u8, vars_buf.written());
        defer allocator.free(vars_json);

        const body = try self.graphqlDo(allocator, mutation, vars_json);
        allocator.free(body); // discard data envelope; only side-effect matters
    }

    /// getProjectV2Fields returns the field definitions on a Project (id + name
    /// + dataType). Caller owns every ProjectField via `deinit`. Used by
    /// projects-v2 to find a "Parent"/"Initiative"/"Tracking" field id from
    /// human-readable candidate names.
    ///
    /// Mirrors Go `GetProjectV2Fields`. The inline-fragment query covers both
    /// `ProjectV2Field` and `ProjectV2SingleSelectField`; fields the query
    /// can't shape are silently skipped (id will be empty after merge).
    pub fn getProjectV2Fields(
        self: *const GithubAdapter,
        allocator: std.mem.Allocator,
        project_node_id: []const u8,
    ) Error![]ProjectField {
        const query =
            \\query GetFields($projectId: ID!) {
            \\  node(id: $projectId) {
            \\    ... on ProjectV2 {
            \\      fields(first: 50) {
            \\        nodes {
            \\          ... on ProjectV2Field { id name dataType }
            \\          ... on ProjectV2SingleSelectField { id name dataType }
            \\        }
            \\      }
            \\    }
            \\  }
            \\}
        ;
        var vars_buf: std.Io.Writer.Allocating = .init(allocator);
        defer vars_buf.deinit();
        const vw = &vars_buf.writer;
        try vw.writeAll("{\"projectId\":");
        try writeJSONString(vw, project_node_id);
        try vw.writeAll("}");
        const vars_json = try allocator.dupe(u8, vars_buf.written());
        defer allocator.free(vars_json);

        const body = try self.graphqlDo(allocator, query, vars_json);
        defer allocator.free(body);

        var parsed = std.json.parseFromSlice(std.json.Value, allocator, body, .{}) catch return Error.ParseFailed;
        defer parsed.deinit();
        const data_v = parsed.value.object.get("data") orelse return Error.ParseFailed;
        if (data_v != .object) return Error.ParseFailed;
        const node_v = data_v.object.get("node") orelse return Error.ParseFailed;
        if (node_v != .object) return &.{};
        const fields_obj_v = node_v.object.get("fields") orelse return &.{};
        if (fields_obj_v != .object) return &.{};
        const nodes_v = fields_obj_v.object.get("nodes") orelse return &.{};
        if (nodes_v != .array) return &.{};

        var out: std.ArrayList(ProjectField) = .empty;
        errdefer {
            for (out.items) |f| f.deinit(allocator);
            out.deinit(allocator);
        }
        for (nodes_v.array.items) |n| {
            if (n != .object) continue;
            const id_text = getObjectString(n.object, "id") orelse continue;
            if (id_text.len == 0) continue;
            const name_text = getObjectString(n.object, "name") orelse "";
            const dt_text = getObjectString(n.object, "dataType") orelse "";
            try out.append(allocator, .{
                .id = try allocator.dupe(u8, id_text),
                .name = try allocator.dupe(u8, name_text),
                .data_type = try allocator.dupe(u8, dt_text),
            });
        }
        return try out.toOwnedSlice(allocator);
    }

    /// freeProjectFields releases every ProjectField in the slice plus the
    /// slice itself. Convenience over manual loop.
    pub fn freeProjectFields(fields: []ProjectField, allocator: std.mem.Allocator) void {
        for (fields) |f| f.deinit(allocator);
        allocator.free(fields);
    }

    pub fn render(_: *const GithubAdapter, allocator: std.mem.Allocator, local: extsync.LocalEntity, _: extsync.CreateOptions) Error![]const u8 {
        const body = if (local.body.len == 0) "(no description)" else local.body;
        const labels = labelsForStatus(local.status);

        var out: std.Io.Writer.Allocating = .init(allocator);
        defer out.deinit();
        const writer = &out.writer;
        try writer.writeAll("{\"title\":");
        try writeJSONString(writer, local.title);
        try writer.writeAll(",\"body\":");
        try writeJSONString(writer, body);
        if (labels) |ls| {
            try writer.writeAll(",\"labels\":[");
            for (ls, 0..) |label, i| {
                if (i != 0) try writer.writeAll(",");
                try writeJSONString(writer, label);
            }
            try writer.writeAll("]");
        }
        try writer.writeAll("}");
        return try allocator.dupe(u8, out.written());
    }
};

const ParsedExternalID = struct {
    owner: []const u8,
    repo: []const u8,
    number: i64,
};

fn parseExternalID(external_id: []const u8) !ParsedExternalID {
    const hash = std.mem.lastIndexOfScalar(u8, external_id, '#') orelse return error.InvalidExternalId;
    if (hash == 0 or hash + 1 >= external_id.len) return error.InvalidExternalId;
    const path = external_id[0..hash];
    const num_str = external_id[hash + 1 ..];
    const slash = std.mem.indexOfScalar(u8, path, '/') orelse return error.InvalidExternalId;
    if (slash == 0 or slash + 1 >= path.len) return error.InvalidExternalId;
    const owner = path[0..slash];
    const repo = path[slash + 1 ..];
    const number = std.fmt.parseInt(i64, num_str, 10) catch return error.InvalidExternalId;
    if (number <= 0) return error.InvalidExternalId;
    return .{ .owner = owner, .repo = repo, .number = number };
}

const StatusMapping = struct {
    state: []const u8,
    labels: ?[]const []const u8 = null,
};

fn stateForStatus(status: []const u8) StatusMapping {
    if (std.mem.eql(u8, status, "todo")) return .{ .state = "open" };
    if (std.mem.eql(u8, status, "doing")) return .{ .state = "open", .labels = &.{"in-progress"} };
    if (std.mem.eql(u8, status, "blocked")) return .{ .state = "open", .labels = &.{"blocked"} };
    if (std.mem.eql(u8, status, "done")) return .{ .state = "closed" };
    if (std.mem.eql(u8, status, "cancelled")) return .{ .state = "closed", .labels = &.{"wontfix"} };
    return .{ .state = "open" };
}

fn labelsForStatus(status: []const u8) ?[]const []const u8 {
    if (std.mem.eql(u8, status, "doing")) return &.{"in-progress"};
    if (std.mem.eql(u8, status, "blocked")) return &.{"blocked"};
    if (std.mem.eql(u8, status, "cancelled")) return &.{"wontfix"};
    return null;
}

fn statusToLocal(state: []const u8, labels: []const []const u8) []const u8 {
    if (std.mem.eql(u8, state, "closed")) {
        for (labels) |label| {
            if (std.mem.eql(u8, label, "wontfix")) return "cancelled";
        }
        return "done";
    }
    for (labels) |label| {
        if (std.mem.eql(u8, label, "in-progress")) return "doing";
        if (std.mem.eql(u8, label, "blocked")) return "blocked";
    }
    return "todo";
}

fn rawStatus(state: []const u8, labels: []const []const u8) []const u8 {
    if (std.mem.eql(u8, state, "closed")) {
        for (labels) |label| {
            if (std.mem.eql(u8, label, "wontfix")) return "closed:wontfix";
        }
        return "closed";
    }
    for (labels) |label| {
        if (std.mem.eql(u8, label, "in-progress")) return "open:in-progress";
        if (std.mem.eql(u8, label, "blocked")) return "open:blocked";
    }
    return "open";
}

fn parseIssue(allocator: std.mem.Allocator, external_id: []const u8, raw: []const u8) Error!extsync.RemoteState {
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return Error.ParseFailed;
    defer parsed.deinit();
    if (parsed.value != .object) return Error.ParseFailed;
    const obj = parsed.value.object;

    const title = getObjectString(obj, "title") orelse "";
    const body = getObjectString(obj, "body") orelse "";
    const state = getObjectString(obj, "state") orelse "open";
    const html_url = getObjectString(obj, "html_url") orelse "";
    const version = getObjectString(obj, "updated_at") orelse "";
    const assignee = blk: {
        const assignee_v = obj.get("assignee") orelse break :blk "";
        if (assignee_v != .object) break :blk "";
        break :blk getObjectString(assignee_v.object, "login") orelse "";
    };

    var labels: std.ArrayList([]const u8) = .empty;
    defer labels.deinit(allocator);
    if (obj.get("labels")) |labels_v| {
        if (labels_v == .array) {
            for (labels_v.array.items) |item| {
                if (item != .object) continue;
                const name = getObjectString(item.object, "name") orelse continue;
                try labels.append(allocator, name);
            }
        }
    }

    const status_local = statusToLocal(state, labels.items);
    const status_raw = rawStatus(state, labels.items);

    return .{
        .external_id = try allocator.dupe(u8, external_id),
        .title = try allocator.dupe(u8, title),
        .body = try allocator.dupe(u8, body),
        .status = try allocator.dupe(u8, status_local),
        .assignee = try allocator.dupe(u8, assignee),
        .priority = 0,
        .due_at = try allocator.dupe(u8, ""),
        .url = try allocator.dupe(u8, html_url),
        .raw_status = try allocator.dupe(u8, status_raw),
        .version = try allocator.dupe(u8, version),
    };
}

fn getObjectString(obj: std.json.ObjectMap, key: []const u8) ?[]const u8 {
    const value = obj.get(key) orelse return null;
    if (value != .string) return null;
    return value.string;
}

fn trimTrailingSlash(s: []const u8) []const u8 {
    if (s.len == 0) return s;
    if (s[s.len - 1] == '/') return s[0 .. s.len - 1];
    return s;
}

fn authHeader(allocator: std.mem.Allocator, cred: extsync.AuthCredential) Error![]const u8 {
    return switch (cred.kind) {
        .bearer => blk: {
            const token = cred.token orelse return Error.InvalidAuth;
            break :blk try std.fmt.allocPrint(allocator, "Bearer {s}", .{token});
        },
        .basic => blk: {
            const user = cred.user orelse return Error.InvalidAuth;
            const pass = cred.pass orelse return Error.InvalidAuth;
            const joined = try std.fmt.allocPrint(allocator, "{s}:{s}", .{ user, pass });
            defer allocator.free(joined);
            const enc_len = std.base64.standard.Encoder.calcSize(joined.len);
            const encoded = try allocator.alloc(u8, enc_len);
            _ = std.base64.standard.Encoder.encode(encoded, joined);
            defer allocator.free(encoded);
            break :blk try std.fmt.allocPrint(allocator, "Basic {s}", .{encoded});
        },
    };
}

fn writeJSONString(writer: *std.Io.Writer, s: []const u8) !void {
    var jsw: std.json.Stringify = .{ .writer = writer, .options = .{} };
    try jsw.write(s);
}

/// buildCreateIssueBody renders the JSON body for `POST /repos/{o}/{r}/issues`.
/// Caller owns the returned slice. Labels are emitted as a JSON array only when
/// non-empty.
fn buildCreateIssueBody(
    allocator: std.mem.Allocator,
    title: []const u8,
    body: []const u8,
    labels: []const []const u8,
) ![]u8 {
    var out: std.Io.Writer.Allocating = .init(allocator);
    defer out.deinit();
    const w = &out.writer;
    try w.writeAll("{\"title\":");
    try writeJSONString(w, title);
    try w.writeAll(",\"body\":");
    try writeJSONString(w, body);
    if (labels.len > 0) {
        try w.writeAll(",\"labels\":[");
        for (labels, 0..) |l, i| {
            if (i != 0) try w.writeAll(",");
            try writeJSONString(w, l);
        }
        try w.writeAll("]");
    }
    try w.writeAll("}");
    return try allocator.dupe(u8, out.written());
}

/// parseCreatedIssue extracts `{number, node_id}` from a GitHub issue-create
/// response body. Returns `ParseFailed` if either field is missing or the wrong
/// shape. Caller owns `node_id`.
fn parseCreatedIssue(allocator: std.mem.Allocator, raw: []const u8) Error!GithubAdapter.CreatedIssue {
    var parsed = std.json.parseFromSlice(std.json.Value, allocator, raw, .{}) catch return Error.ParseFailed;
    defer parsed.deinit();
    if (parsed.value != .object) return Error.ParseFailed;
    const obj = parsed.value.object;
    const num_v = obj.get("number") orelse return Error.ParseFailed;
    const number: i64 = switch (num_v) {
        .integer => |n| n,
        else => return Error.ParseFailed,
    };
    // node_id is optional in some response surfaces but Go relies on it for
    // ProjectsV2 wiring; default to "" if absent so parent-issue parity still
    // works (we don't need node_id for sub-issue linking).
    const node_id_text: []const u8 = blk: {
        const v = obj.get("node_id") orelse break :blk "";
        if (v != .string) break :blk "";
        break :blk v.string;
    };
    const node_id_owned = try allocator.dupe(u8, node_id_text);
    return .{ .number = number, .node_id = node_id_owned };
}

test "validate enforces owner/repo#number format" {
    const fake = extsync.Transport{ .ctx = undefined, .sendFn = unreachableSend };
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, fake);
    try adapter.validate("acme/api#42");
    try std.testing.expectError(Error.InvalidExternalId, adapter.validate("PROJ-1"));
    try std.testing.expectError(Error.InvalidExternalId, adapter.validate("acme/api"));
}

test "pull maps issue state and labels to local status" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"number":42,"title":"Fix widget","body":"Issue body","state":"open","html_url":"https://github.com/acme/api/issues/42","assignee":{"login":"octocat"},"labels":[{"name":"in-progress"}]}
    );
    defer t.deinit();

    const adapter = GithubAdapter.init("https://api.github.com", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const state = try adapter.pull(a, "acme/api#42");
    defer extsync.deinitRemoteState(state, a);

    try std.testing.expectEqualStrings("acme/api#42", state.external_id);
    try std.testing.expectEqualStrings("Fix widget", state.title);
    try std.testing.expectEqualStrings("doing", state.status);
    try std.testing.expectEqualStrings("open:in-progress", state.raw_status);
    try std.testing.expectEqualStrings("octocat", state.assignee);
}

test "push sends PATCH with status mapping" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200, "{}");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());

    const out = try adapter.push(a, "acme/api#42", .{ .status = "done", .title = "Updated title" });
    defer extsync.deinitUpdateOutcome(out, a);
    try std.testing.expectEqual(@as(usize, 1), t.calls);
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"state\":\"closed\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"title\":\"Updated title\"") != null);
}

test "render emits label set for doing status" {
    const a = std.testing.allocator;
    const fake = extsync.Transport{ .ctx = undefined, .sendFn = unreachableSend };
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, fake);
    const payload = try adapter.render(a, .{
        .kind = "task",
        .id = 7,
        .title = "T",
        .body = "B",
        .status = "doing",
    }, .{});
    defer a.free(payload);
    try std.testing.expect(std.mem.indexOf(u8, payload, "\"labels\":[\"in-progress\"]") != null);
}

const FakeTransport = struct {
    allocator: std.mem.Allocator,
    status: u16,
    body: []const u8,
    calls: usize = 0,
    last_body: ?[]const u8 = null,
    last_url: ?[]const u8 = null,

    fn init(allocator: std.mem.Allocator, status: u16, body: []const u8) FakeTransport {
        return .{ .allocator = allocator, .status = status, .body = body };
    }

    fn deinit(self: *FakeTransport) void {
        if (self.last_body) |b| self.allocator.free(b);
        if (self.last_url) |u| self.allocator.free(u);
    }

    fn transport(self: *FakeTransport) extsync.Transport {
        return .{ .ctx = self, .sendFn = send };
    }

    fn send(ctx: *anyopaque, allocator: std.mem.Allocator, req: extsync.Request) anyerror!extsync.Response {
        const self: *FakeTransport = @ptrCast(@alignCast(ctx));
        self.calls += 1;
        if (self.last_body) |old| self.allocator.free(old);
        if (self.last_url) |old| self.allocator.free(old);
        self.last_body = if (req.body) |b| try self.allocator.dupe(u8, b) else null;
        self.last_url = try self.allocator.dupe(u8, req.url);
        return .{
            .status = self.status,
            .body = try allocator.dupe(u8, self.body),
        };
    }
};

fn unreachableSend(_: *anyopaque, _: std.mem.Allocator, _: extsync.Request) anyerror!extsync.Response {
    return error.UnreachableTransport;
}

test "createIssue parses number and node_id from REST response" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 201,
        \\{"number":42,"node_id":"NODE_42","html_url":"https://github.com/acme/api/issues/42"}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const created = try adapter.createIssue(a, "acme", "api", "Hello", "World", &.{});
    defer a.free(created.node_id);
    try std.testing.expectEqual(@as(i64, 42), created.number);
    try std.testing.expectEqualStrings("NODE_42", created.node_id);
    // The POST body should carry title + body and omit labels when empty.
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"title\":\"Hello\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"body\":\"World\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "labels") == null);
}

test "createIssue emits labels array when present" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 201,
        \\{"number":7,"node_id":"NODE_7"}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const labels = [_][]const u8{ "bug", "p1" };
    const created = try adapter.createIssue(a, "o", "r", "T", "B", &labels);
    defer a.free(created.node_id);
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"labels\":[\"bug\",\"p1\"]") != null);
}

test "linkSubIssue 404 maps to NotFound" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 404, "Not Found");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try std.testing.expectError(Error.NotFound, adapter.linkSubIssue(a, "o", "r", 1, 2));
}

test "linkSubIssue 201 success" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 201, "{\"id\":2}");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try adapter.linkSubIssue(a, "o", "r", 1, 2);
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"sub_issue_id\":2") != null);
}

test "linkSubIssueProbe 404 means endpoint not enabled" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 404, "Not Found");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try std.testing.expectError(Error.NotFound, adapter.linkSubIssueProbe(a, "o", "r"));
}

test "linkSubIssueProbe 422 means endpoint enabled" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 422, "Unprocessable");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try adapter.linkSubIssueProbe(a, "o", "r");
}

test "postComment POSTs to comments endpoint with body" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 201, "{\"id\":1}");
    defer t.deinit();
    const adapter = GithubAdapter.init("https://api.github.com", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try adapter.postComment(a, "acme/api#42", "Hello \"world\"");
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "Hello \\\"world\\\"") != null);
}

// ---- GraphQL surface tests (Cycle B''' / task 2349) -------------------------

test "getAuthenticatedOwner parses viewer + orgs" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"viewer":{"id":"U1","organizations":{"nodes":[{"id":"O1"},{"id":"O2"}]}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("https://api.github.com", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const owner = try adapter.getAuthenticatedOwner(a);
    defer owner.deinit(a);
    try std.testing.expectEqualStrings("U1", owner.user_node_id);
    try std.testing.expectEqual(@as(usize, 2), owner.org_ids.len);
    try std.testing.expectEqualStrings("O1", owner.org_ids[0]);
    try std.testing.expectEqualStrings("O2", owner.org_ids[1]);
    // Confirms /graphql endpoint.
    try std.testing.expect(t.last_url != null);
    try std.testing.expect(std.mem.endsWith(u8, t.last_url.?, "/graphql"));
}

test "getAuthenticatedOwner tolerates missing organizations field" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200, "{\"data\":{\"viewer\":{\"id\":\"U1\"}}}");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const owner = try adapter.getAuthenticatedOwner(a);
    defer owner.deinit(a);
    try std.testing.expectEqualStrings("U1", owner.user_node_id);
    try std.testing.expectEqual(@as(usize, 0), owner.org_ids.len);
}

test "createProjectV2 parses projectV2.id + url" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"createProjectV2":{"projectV2":{"id":"PROJ_1","url":"https://github.com/users/alice/projects/1"}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const created = try adapter.createProjectV2(a, "OWNER_1", "Plan Title");
    defer created.deinit(a);
    try std.testing.expectEqualStrings("PROJ_1", created.node_id);
    try std.testing.expectEqualStrings("https://github.com/users/alice/projects/1", created.url);
    // The request body must carry ownerId + title variables.
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"ownerId\":\"OWNER_1\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"title\":\"Plan Title\"") != null);
    // GraphQL query body must be present (inside the outer "query" json string).
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "createProjectV2") != null);
}

test "addProjectV2Item returns assigned item id" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"addProjectV2ItemById":{"item":{"id":"ITEM_42"}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const item_id = try adapter.addProjectV2Item(a, "PROJ_1", "CONTENT_1");
    defer a.free(item_id);
    try std.testing.expectEqualStrings("ITEM_42", item_id);
}

test "setProjectV2ItemFieldValue sends text value variable" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"updateProjectV2ItemFieldValue":{"projectV2Item":{"id":"ITEM_42"}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try adapter.setProjectV2ItemFieldValue(a, "PROJ_1", "ITEM_42", "FIELD_1", "acme/api#7");
    try std.testing.expect(t.last_body != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"text\":\"acme/api#7\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "\"fieldId\":\"FIELD_1\"") != null);
}

test "getProjectV2Fields returns parsed field nodes" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"node":{"fields":{"nodes":[
        \\{"id":"F1","name":"Parent","dataType":"TEXT"},
        \\{"id":"F2","name":"Status","dataType":"SINGLE_SELECT"},
        \\{}
        \\]}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const fields = try adapter.getProjectV2Fields(a, "PROJ_1");
    defer GithubAdapter.freeProjectFields(fields, a);
    try std.testing.expectEqual(@as(usize, 2), fields.len);
    try std.testing.expectEqualStrings("F1", fields[0].id);
    try std.testing.expectEqualStrings("Parent", fields[0].name);
    try std.testing.expectEqualStrings("TEXT", fields[0].data_type);
    try std.testing.expectEqualStrings("F2", fields[1].id);
}

test "graphqlDo surfaces GraphqlError when errors array non-empty" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"errors":[{"message":"oops"}],"data":null}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try std.testing.expectError(Error.GraphqlError, adapter.getAuthenticatedOwner(a));
}

test "graphqlDo maps HTTP non-200 to UnexpectedStatus" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 500, "Server Error");
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    try std.testing.expectError(Error.UnexpectedStatus, adapter.getAuthenticatedOwner(a));
}

test "graphqlDo escapes operator-controlled strings in variables" {
    const a = std.testing.allocator;
    var t = FakeTransport.init(a, 200,
        \\{"data":{"createProjectV2":{"projectV2":{"id":"P1","url":""}}}}
    );
    defer t.deinit();
    const adapter = GithubAdapter.init("", .{ .kind = .bearer, .token = "tok" }, t.transport());
    const created = try adapter.createProjectV2(a, "OWNER", "Title with \" and \\");
    defer created.deinit(a);
    try std.testing.expect(t.last_body != null);
    // The injected " must be JSON-escaped; raw " must not appear unescaped
    // inside the variable string. Look for the escaped form.
    try std.testing.expect(std.mem.indexOf(u8, t.last_body.?, "Title with \\\" and \\\\") != null);
}
