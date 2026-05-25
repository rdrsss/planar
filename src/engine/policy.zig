//! engine/policy — invariants every data-plane mutation must honor.
//!
//! Three sub-modules, each enforcing one cross-cutting rule:
//!
//!   scope_guard — refuse cross-scope writes (cross-scope-guard rule).
//!   status      — refuse illegal status transitions per entity kind.
//!   audit       — record one row per data-plane mutation.
//!
//! Data-plane modules (engine.planning.*, engine.runtime.*, ...) call
//! these at the top + bottom of every mutation. Keeping the rules in
//! one place means they can be tested in isolation and changed without
//! editing every CRUD function.
//!
//! Policy modules MUST NOT import data-plane modules. They may borrow
//! types from the identity (control-plane) layer, but most types are
//! redeclared locally to keep policy independently testable.

pub const scope_guard = @import("policy/scope_guard.zig");
pub const status = @import("policy/status.zig");
pub const audit = @import("policy/audit.zig");

test {
    _ = scope_guard;
    _ = status;
    _ = audit;
}
