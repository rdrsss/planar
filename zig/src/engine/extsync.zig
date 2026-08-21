//! engine/extsync - adapter namespace.

pub const common = @import("extsync/common.zig");
pub const jira = @import("extsync/jira.zig");
pub const github = @import("extsync/github.zig");
pub const propagate = @import("extsync/propagate.zig");
pub const strategy = @import("extsync/strategy.zig");
pub const parent_issue = @import("extsync/parent_issue.zig");
pub const projects_v2 = @import("extsync/projects_v2.zig");

test {
    _ = common;
    _ = jira;
    _ = github;
    _ = propagate;
    _ = strategy;
    _ = parent_issue;
    _ = projects_v2;
}
