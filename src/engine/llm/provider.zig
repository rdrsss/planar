//! engine/llm/provider — provider strategy resolver for LLM handoff flows.
//!
//! M18 keeps import/synthesize no-network by default: the provider is
//! metadata for the staged request/result contract, not an in-process SDK call.

const std = @import("std");

pub const Kind = enum {
    shell,
    anthropic,
    openai,

    pub fn text(self: Kind) []const u8 {
        return switch (self) {
            .shell => "shell",
            .anthropic => "anthropic",
            .openai => "openai",
        };
    }
};

pub fn parse(raw: []const u8) !Kind {
    if (std.mem.eql(u8, raw, "shell")) return .shell;
    if (std.mem.eql(u8, raw, "anthropic")) return .anthropic;
    if (std.mem.eql(u8, raw, "openai")) return .openai;
    return error.InvalidInput;
}

pub fn resolve(explicit: ?[]const u8, environ: std.process.Environ) !Kind {
    if (explicit) |raw| {
        if (raw.len == 0) return .shell;
        return parse(raw);
    }
    if (environ.getPosix("PLANAR_LLM_PROVIDER")) |raw| {
        if (raw.len == 0) return .shell;
        return parse(raw);
    }
    return .shell;
}

test "parse accepts supported provider names" {
    try std.testing.expectEqual(Kind.shell, try parse("shell"));
    try std.testing.expectEqual(Kind.anthropic, try parse("anthropic"));
    try std.testing.expectEqual(Kind.openai, try parse("openai"));
}

test "parse rejects unknown provider names" {
    try std.testing.expectError(error.InvalidInput, parse("azure"));
}
