//! `db` module root: re-exports the SQLite wrapper and migration runner.
//! Importing this module pulls in both submodules' tests when targeted by
//! `zig build test`.
pub const sqlite = @import("sqlite.zig");
pub const migrate = @import("migrate.zig");

test {
    _ = sqlite;
    _ = migrate;
}
