//! engine/llm — provider resolution + cache/client helpers for LLM handoff.

pub const provider = @import("llm/provider.zig");
pub const client = @import("llm/client.zig");
pub const evidence = @import("llm/evidence.zig");

test {
    _ = provider;
    _ = client;
    _ = evidence;
}
