//! metrics/metrics_sql.zig — build-time module that embeds the checked-in
//! SQL metric queries as Zig string constants.
//!
//! This module is the build bridge between the analyst-facing SQL files in
//! metrics/ and the unit tests that need to execute them in-process. The
//! analyst-facing files remain the single source of truth; this module
//! embeds them so tests can read and run the SQL without file-path
//! resolution at test runtime.

/// Content of metrics/rq1_touch_accuracy.sql (M-PREC / M-REC, preregistration §2).
pub const rq1_touch_accuracy: []const u8 = @embedFile("rq1_touch_accuracy.sql");
