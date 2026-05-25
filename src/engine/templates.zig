//! engine/templates — JSON template engine.
//!
//! Mirrors `src/internal/templates/`. Loads templates from disk with a
//! three-level fallback chain (user-set → default-set → embedded), renders
//! `{{.Field}}` substitutions against a Context built from DB entities, and
//! validates template syntax.
//!
//! Template files are JSON. String values may contain Go-template-style
//! directives (`{{.Plan.Title}}`, `{{range .Touches}}...{{end}}`,
//! `{{if .ExternalKey}}...{{end}}`) which the renderer substitutes against
//! the supplied Context. Non-string values pass through unchanged.

pub const loader = @import("templates/loader.zig");
pub const render = @import("templates/render.zig");
pub const validate_mod = @import("templates/validate.zig");
pub const init_mod = @import("templates/init.zig");
pub const context = @import("templates/context.zig");
pub const builder = @import("templates/builder.zig");

pub const Template = loader.Template;
pub const ListEntry = loader.ListEntry;
pub const load = loader.load;
pub const listEntries = loader.listEntries;
pub const listEmbeddedEntries = loader.listEmbeddedEntries;
pub const deinitTemplate = loader.deinitTemplate;
pub const deinitListEntries = loader.deinitListEntries;

pub const Context = context.Context;
pub const PlanInfo = context.PlanInfo;
pub const TaskInfo = context.TaskInfo;
pub const ScenarioInfo = context.ScenarioInfo;
pub const AssocInfo = context.AssocInfo;
pub const ChildRef = context.ChildRef;

pub const renderTemplate = render.renderTemplate;

pub const ValidationIssue = validate_mod.ValidationIssue;
pub const validate = validate_mod.validate;
pub const deinitIssues = validate_mod.deinitIssues;

pub const initOnDisk = init_mod.initOnDisk;

test {
    _ = loader;
    _ = render;
    _ = validate_mod;
    _ = init_mod;
    _ = context;
    _ = builder;
}
