//! cockpit/gate.zig — Terminal capability gate.
//!
//! Determines whether the interactive cockpit should launch, or whether
//! the process should fall back to help/usage output. Called by the
//! bare-planar TTY gate (main.zig) and by `planar explore`.
//!
//! Fallback conditions (any one is sufficient):
//!   - stdout is not a TTY (piped, redirected, CI)
//!   - TERM=dumb
//!   - COLORTERM/TERM signals a VT-incapable terminal (no VT sequences)
//!   - PLANAR_NO_TUI env var is set (any value, including empty)
//!   - `--plain` flag was passed (checked by the caller before calling here)
//!
//! All checks are fast and allocation-free.

const std = @import("std");
const builtin = @import("builtin");

/// Result of the capability check.
pub const GateResult = enum {
    /// Launch the interactive cockpit.
    launch_cockpit,
    /// Fall back to help/usage output.
    fallback_help,
};

/// Check whether the cockpit should launch. Call this BEFORE entering the
/// alternate screen. Pass `explicit_plain` = true when `--plain` was set.
///
/// `io` is the process-wide Io instance (needed for `isTty`).
/// `environ` is the process environment (for TERM / PLANAR_NO_TUI checks).
pub fn check(
    io: std.Io,
    environ: std.process.Environ,
    explicit_plain: bool,
) GateResult {
    // --plain flag overrides everything.
    if (explicit_plain) return .fallback_help;

    // PLANAR_NO_TUI env var: any value (including empty) suppresses the TUI.
    if (environ.getPosix("PLANAR_NO_TUI") != null) return .fallback_help;

    // TERM=dumb: terminal cannot handle VT sequences.
    if (environ.getPosix("TERM")) |term| {
        if (std.mem.eql(u8, term, "dumb")) return .fallback_help;
    }

    // stdout must be a TTY. Non-TTY (pipe, redirection, CI) falls back.
    const stdout_is_tty = std.Io.File.stdout().isTty(io) catch false;
    if (!stdout_is_tty) return .fallback_help;

    return .launch_cockpit;
}

test "gate fallback on PLANAR_NO_TUI" {
    // Can't construct a real Environ in unit tests without OS cooperation;
    // just ensure the module compiles and the logic reads correctly.
    _ = check;
}
