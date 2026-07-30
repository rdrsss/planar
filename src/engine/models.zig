//! engine/models.zig — opaque candidate configuration and host observations.
//!
//! Backs the `planar models` verb. Two responsibilities:
//!
//!   1. A curated, in-repo catalog of the model identifiers each supported
//!      provider CLI exposes, classified into the canonical `small` / `medium`
//!      / `large` tiers (plan 541). The provider CLIs (`claude`, `codex`) do
//!      NOT expose a machine-readable "list models" command, so enumeration is
//!      impossible — the catalog is the authoritative known-models source and
//!      is maintained here by hand.
//!
//!   2. Local discovery: probe whether each provider CLI is installed by
//!      actually invoking `<bin> --version` (instant + auth-free for both
//!      claude and codex), recording installed-state + version. This is the
//!      "call claude and codex" half; it confirms callability, while the model
//!      list comes from the curated catalog.
//!
//! ## Tier model (plan 541)
//!
//! Roles map to tiers, tiers map to per-vendor models. The default routing here
//! (coder→medium, reviewer→large, the rest→medium). The plan-540 shared resolver
//! (phase 2) is the eventual single source of truth for external harnesses and
//! skills render.
//!
//! The embedded catalog below is retained for one read/export compatibility
//! window only. It is never an eligibility, classification, or ranking input.

const std = @import("std");
const config = @import("config.zig");

// ---------------------------------------------------------------------------
// Shared model resolver (plan 540 phase 2) — the single authority that maps
// (vendor, role|tier) to a concrete model, reading the effective config
// (models.<vendor>.<tier> tier maps + roles.<role> role→tier) produced by
// engine.config.resolve(). Consumers (external harnesses, skills render,
// `planar models`) resolve through this instead of carrying their own tables.
// ---------------------------------------------------------------------------

/// Failure modes of the resolver.
pub const ResolveError = error{
    /// No `models.<vendor>.*` keys exist for the requested vendor.
    UnknownVendor,
    /// The vendor is known but has no model at the requested tier.
    UnknownTier,
    /// No `roles.<role>` mapping exists for the requested role.
    UnknownRole,
    /// The tier key exists but its model id is empty.
    MissingModel,
};

/// A resolved (vendor, tier) → model with provenance. `tier`/`model` borrow
/// from the effective map and live as long as it does.
pub const Resolution = struct {
    vendor: []const u8,
    tier: []const u8,
    model: []const u8,
    source: config.Provenance,
};

pub const HostObservationView = struct {
    host_id: []const u8,
    observation_version: i64,
    cli_availability: []const u8,
    exact_spawn_verification: []const u8,
    captured_at: []const u8,
    expires_at: []const u8,
    evidence_ref: []const u8,
};

pub const ConfiguredCandidate = struct {
    opaque_id: []const u8,
    vendor: []const u8,
    enabled: bool = true,
    tier: []const u8,
    allowed_roles: []const []const u8,
    fallback_order: usize,
    compatibility_source: []const u8 = "legacy_config",
    latest_observation: ?HostObservationView = null,
};

pub const ConfiguredRegistry = struct {
    candidates: []ConfiguredCandidate,
    migration_warning: []const u8 =
        "legacy [models] configuration imported as opaque candidate IDs; export and migrate to the candidate registry",

    pub fn deinit(self: ConfiguredRegistry, allocator: std.mem.Allocator) void {
        for (self.candidates) |candidate| allocator.free(candidate.allowed_roles);
        allocator.free(self.candidates);
    }
};

/// Canonical capability tiers (plan 541). `small` = cheap/fast, `large` =
/// heavy reasoning.
pub const Tier = enum {
    small,
    medium,
    large,

    pub fn name(self: Tier) []const u8 {
        return switch (self) {
            .small => "small",
            .medium => "medium",
            .large => "large",
        };
    }
};

// ---------------------------------------------------------------------------
// Unit tests
// ---------------------------------------------------------------------------

const testing = std.testing;

// ---------------------------------------------------------------------------
// Resolver unit tests (plan 540 phase 2 / task 3621)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// Work-type routing resolver tests (plan 899, task worktype-routing-map)
// ---------------------------------------------------------------------------
