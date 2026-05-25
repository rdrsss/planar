//! OS-isolated platform layer for the cli library.
//!
//! Currently only houses argv acquisition. The Zig stdlib's
//! `std.process.argsAlloc` already handles the POSIX (zero-alloc-ish)
//! vs Windows (UTF-16 → UTF-8 with allocation) split, so the platform
//! files here are thin wrappers. The directory exists so future
//! OS-specific helpers (terminal width detection, config-dir resolution,
//! signal handling, etc.) have a clear home without polluting the
//! parser surface.

const builtin = @import("builtin");

const impl = switch (builtin.os.tag) {
    .windows => @import("windows.zig"),
    else => @import("posix.zig"),
};

pub const argv = impl.argv;
pub const freeArgv = impl.freeArgv;
