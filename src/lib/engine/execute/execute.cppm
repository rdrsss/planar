/// @file execute.cppm
/// @brief `planar.engine_execute` — the sandboxed, spawn-free Lua workflow
/// host behind `planar-execute run` (plan 996, task 6042; plan 633 for the
/// binary itself).
///
/// Port target: `zig/src/cmd/planar-execute/host.zig` plus `runWorkflow` in
/// `zig/src/cmd/planar-execute/main.zig`.
///
/// ## THE EXPORTED SURFACE OF THIS MODULE IS ITSELF A BOUNDARY
///
/// Read the export list below and notice what is NOT on it: no `lua_State`,
/// no way to obtain one, no way to register a function into one, and above
/// all no process-spawning primitive. This module DOES fork and exec — it
/// has to, because `cli.planar(...)` and `git.head_sha()` are shells — but
/// that runner is defined inside `host.cpp` with internal linkage and is
/// unreachable from outside this module by construction. There is no header
/// to include and no exported declaration to name. A future caller that
/// wants "just a small exec helper, it's already written" cannot have one
/// without deliberately exporting it, and that edit is visible in this file.
///
/// That matters more here than anywhere else in the tree. An earlier
/// planar-execute grew re-entrant headless LLM spawning and became a harness
/// in its own right, which is why it was extracted to a separate project;
/// this revival exists to hold that line (plan 633 D5). The three
/// independent locks are:
///
///   1. CONFIGURE TIME — `cmake/architecture.cmake` treats this target as an
///      execute carrier and FATALs if it (or anything reaching it) reaches
///      `planar_db`. Adding `db` to DEPENDS fails the build with a named
///      diagnostic; a test could only observe a handle this module chose to
///      open, so the guard sits earlier than any test can.
///   2. COMPILE TIME — `allowed_host_fns()` and `denied_host_fns()` are
///      `constexpr` tables and `manifest.cpp` carries a `static_assert` that
///      their name sets are disjoint. A registrar entry named `spawn` or
///      `exec` cannot compile.
///   3. RUN TIME — `registered_host_surface()` builds a real state, installs
///      the surface, and enumerates what is ACTUALLY reachable from a
///      workflow. `surface.t.cpp` compares that live enumeration against the
///      frozen manifest. See "the frozen manifest" below for why that is a
///      different and stronger claim than comparing two constants.
///
/// ## The frozen manifest, and what its test actually proves
///
/// `allowed_host_fns()` is a table of `(table, name)` pairs — the ENTIRE
/// host-call capability surface of a workflow. Twenty-five entries, and the
/// number is pinned.
///
/// The test that matters is not "does `fs.read` read a file". It is
/// `registered_host_surface() == allowed_host_fns()`, where the left side is
/// produced by walking the five global tables of a live `lua_State` with
/// `lua_next` after `install_host_surface` has run. That comparison fails on
/// every way the surface can grow or shift:
///
///   - a new host function registered but not declared (the growth case the
///     whole milestone guards against — the registrar is data-driven off the
///     manifest, so this specifically catches a hand-written `lua_setfield`
///     added beside the loop);
///   - a declared function silently not registered (a dispatch arm dropped);
///   - a function moved between tables (`fs.read` becoming `cli.read`);
///   - a sixth global table appearing;
///   - a non-function value appearing on one of the five tables (caught
///     separately, since `ctx` legitimately carries three non-function
///     fields and the enumeration is typed).
///
/// What it does NOT prove: that any individual host function is correct.
/// That is deliberate and is the milestone's stated priority — the risk this
/// binary carries is the surface quietly widening, not `git.clean` gaining
/// an off-by-one.
///
/// ## Sandbox
///
/// `open_sandboxed_libs` opens base, table, string, math and utf8, then
/// removes the escape hatches. Every removal below was derived by RUNNING
/// the oracle and enumerating its live globals (`_G` and each table), never
/// read out of the Zig source:
///
///   nil globals:   dofile  load  loadfile  require  (plus `loadstring`,
///                  which 5.5 does not define anyway)
///   never opened:  os  io  debug  coroutine  package
///   nil'd fields:  math.random  math.randomseed
///
/// `coroutine` is absent rather than trimmed: the hand-back model is one
/// clean process per phase, and a coroutine park awaiting an external worker
/// is exactly where re-entrant spawning regrew last time.
///
/// With `os` and `io` gone the only clock and the only seed a workflow can
/// reach are `ctx.now` and `ctx.seed`, both injected by the caller and both
/// defaulting to 0 — so a run is reproducible by default.
///
/// `sandbox_globals()` and `sandbox_table_entries()` exist so that list can
/// be asserted against a live state instead of against this comment.
module;

export module planar.engine_execute;

import std;

namespace planar::engine::execute {

// ---------------------------------------------------------------------------
// The frozen manifest
// ---------------------------------------------------------------------------

/// @brief One registered host function: the table that owns it and its field
/// name. Host functions are never globals — a workflow reaches the host only
/// through `cli.*` / `git.*` / `fs.*` / `flow.*` / `ctx.*`.
export struct host_fn {
  std::string_view table; ///< Owning table: cli / git / fs / flow / ctx.
  std::string_view name;  ///< Field name on that table.

  /// @brief Value equality, so a live enumeration can be compared to the
  /// manifest directly.
  /// @param other The other entry.
  /// @return `true` when both halves match.
  friend auto operator==(host_fn const& lhs, host_fn const& other) -> bool = default;
};

/// @brief The five host tables, in registration order.
/// @return The table names.
export auto host_tables() -> std::span<const std::string_view>;

/// @brief The complete host-call capability surface (D7's allowlist).
///
/// The single source of truth: the registrar iterates THIS to build the
/// tables, so an implementation cannot drift from the declaration in the
/// "declared but absent" direction, and `surface.t.cpp`'s live enumeration
/// closes the "present but undeclared" direction.
/// @return The manifest, sorted by (table, name) within each table group.
export auto allowed_host_fns() -> std::span<const host_fn>;

/// @brief Names that must never appear anywhere on the host surface — the
/// spawn / general-exec primitives that grew this binary into a harness.
///
/// Asserted disjoint from `allowed_host_fns()` by a `static_assert`, and
/// asserted absent from a LIVE state by `surface.t.cpp` (which checks the
/// globals too, not just the five tables — a `spawn` global would be just as
/// reachable as a `cli.spawn`).
/// @return The denylist.
export auto denied_host_fns() -> std::span<const std::string_view>;

/// @brief The only binaries a `cli.*` host function may shell.
/// @return `planar`, `planar-agent`, `planar-watch`.
export auto allowed_cli_bins() -> std::span<const std::string_view>;

// ---------------------------------------------------------------------------
// Pure guards (no Lua, no process, no filesystem)
// ---------------------------------------------------------------------------

/// @brief Is `<bin> <argv...>` inside the deterministic workflow capability
/// set?
///
/// A SECOND allowlist behind the binary allowlist: being allowed to shell
/// `planar` is not being allowed to run every `planar` verb. `planar`
/// commands are matched as a `(verb, subcommand)` pair (plus four
/// single-token verbs); `planar-agent` and `planar-watch` match on the first
/// token alone.
/// @param bin The binary name (not a path).
/// @param argv The arguments after the binary name.
/// @return `true` when the command may run.
export auto command_allowed(std::string_view bin, std::span<const std::string> argv) -> bool;

/// @brief Does `value` look like a git object id (7–64 hex characters)?
/// @param value The candidate.
/// @return `true` when it does.
export auto is_hex_object_id(std::string_view value) -> bool;

/// @brief Is `value` safe to pass to git as a ref?
///
/// Rejects the empty string, anything starting `-` (which git would read as
/// an option), anything containing `..` (the range syntax, and the parent
/// component), and any character outside `[A-Za-z0-9/_.~^-]`.
/// @param value The candidate ref.
/// @return `true` when it is safe.
export auto safe_git_ref(std::string_view value) -> bool;

/// @brief Is `rel` an acceptable sandbox-relative path?
///
/// Rejects empty, absolute, backslash-bearing, and any path with an empty,
/// `.` or `..` component. This is the CHEAP half of `fs.*` confinement; the
/// expensive half is the component-by-component no-follow directory walk in
/// `host.cpp`, which is what actually defeats a symlink swapped in after
/// this check passes.
/// @param rel The candidate relative path.
/// @return `true` when the shape is acceptable.
export auto validate_confined_rel(std::string_view rel) -> bool;

/// @brief Format a double the way the oracle's `{d}` does: shortest
/// round-tripping decimal, never scientific notation.
///
/// Split out and exported because it is the one piece of `flow.result`
/// marshalling with a non-obvious contract and it is worth pinning directly.
/// The oracle prints `1e300` as three hundred and one digits and `3e-7` as
/// `0.0000003`; `std::format("{}", d)` would print `1e+300` and `3e-07`.
/// @param value The number.
/// @return The formatted text.
export auto format_double(double value) -> std::string;

// ---------------------------------------------------------------------------
// state — planner state-read parsing (port target: state.zig)
// ---------------------------------------------------------------------------

/// @brief Pure JSON-decode helpers for the planner reads `ctx.brief` needs.
///
/// Port target: `zig/src/cmd/planar-execute/state.zig`. Only the slice that
/// feeds `ctx.brief` is ported — `planShow`, `taskShow`, and the
/// `TaskPacket` family. The oracle's other `state.zig` helpers
/// (`recommendStrategy`'s typed struct, `contextList`, `claimStatus`,
/// `testSpecStatus`, `questionAdd`, `taskTouchesList`'s typed struct) exist
/// there ONLY to serve `main.zig`'s retired orchestration loop
/// (`agent`/`parallel`/`pipeline`/`workflow` primitives) — the very
/// re-entrant harness plan 633 D5 exists to keep out of this binary. The
/// registered `ctx.plan_show` / `ctx.task_show` / `ctx.task_touches` /
/// `ctx.recommend_strategy` host functions confirm this: in the CURRENT
/// oracle (`host.zig`) all four shell generically and push raw parsed JSON,
/// never touching these typed structs. Porting them here would be
/// dead code with no consumer on the ported host surface.
///
/// Every read is PURE (no subprocess call): the caller shells the binary via
/// `run_allowlisted` in `host.cpp` and hands the captured stdout to the
/// parser below, mirroring `state.zig`'s own `spawnPlanar` / parse split
/// (`parseTaskPacket` was already factored out there for the same reason:
/// testability without a live `planar`).
namespace state {

/// @brief Why a `state::parse_*` call failed.
///
/// One enumerator, matching `schema_parse_error` and `json_dom::json_parse_error`'s
/// reasoning: every caller here maps any failure to the same oracle-shaped
/// diagnostic (`ctx.brief: plan <id> not found`, etc.), so a richer error set
/// would invite a distinction no caller makes.
export enum class state_parse_error : std::uint8_t {
  malformed, ///< The bytes are not valid JSON, or a required field is
             ///< missing or the wrong type.
};

/// @brief Minimal view of `planar plan show <id> --json`. Port target:
/// `state.zig`'s `PlanShow`.
export struct plan_show {
  std::int64_t                id = 0;         ///< The plan's row id, as passed to `plan show`.
  std::string                 title;          ///< The plan's human-readable title, quoted into the brief header.
  std::string                 status;         ///< One of `draft`, `active`, `paused`, `done`, `abandoned`.
  std::optional<std::string>  slug;           ///< The `"slug"` field; unset when the plan has none.
  std::optional<std::int64_t> parent_plan_id; ///< The `"parent_plan_id"` field; unset for a top-level plan.
};

/// @brief Minimal view of `planar task show <id> --json`. Port target:
/// `state.zig`'s `TaskShow`.
export struct task_show {
  std::int64_t               id = 0; ///< The task's row id, as passed to `task show`.
  std::optional<std::string> slug;   ///< The `"slug"` field; unset when the task has none.
  std::string                status; ///< One of `todo`, `doing`, `blocked`, `done`, `cancelled`.
};

/// @brief One task, adapted for `brief::brief_inputs::tasks`. Port target:
/// `state.zig`'s `TaskEntry`.
export struct task_entry {
  std::int64_t               id      = 0; ///< The task's row id.
  std::int64_t               plan_id = 0; ///< The `"plan_id"` field — the owning plan's id.
  std::string                title;       ///< The task's human-readable title, rendered as the brief's work item.
  std::optional<std::string> slug;        ///< The `"slug"` field; unset when the task has none.
  std::string                status;      ///< One of `todo`, `doing`, `blocked`, `done`, `cancelled`.
};

/// @brief One row of `planar task packet <id> --json`'s evidence arrays.
/// Port target: `state.zig`'s `PacketEvidence`.
export struct packet_evidence {
  std::string  kind;                 ///< The `"kind"` field — the evidence category, e.g. `"citation"`, `"decision"`, `"claim"`.
  std::int64_t id = 0;               ///< The `"id"` field — the referenced entity's id within `kind`.
  std::string  locator;              ///< The `"locator"` field — where the evidence lives (spec path, decision id, etc.).
  std::string  text;                 ///< The `"text"` field — the verbatim evidence text quoted into the brief; for a claim
                                     ///< row this is the claim token (see `compile_brief`'s claim-token match).
  std::string  display_label;        ///< The `"display_label"` field; empty when the caller has no friendlier label
                                     ///< than `text`/`locator` to show.
  std::string  source_digest;        ///< The `"source_digest"` field — the evidence's digest as of packet materialization.
  std::string  current_digest;       ///< The `"current_digest"` field — the evidence's digest as it stands now; differs
                                     ///< from `source_digest` when the underlying source has drifted since materialization.
  bool         required = false;     ///< The `"required"` field — whether the packet treats this row as mandatory
                                     ///< for `task_packet::ready()`.
  bool         covered  = false;     ///< The `"covered"` field — whether this row's coverage obligation is satisfied.
  std::string  status;               ///< The `"status"` field; for a claim row, `"active"` marks the live claim
                                     ///< (see `compile_brief`'s claim lookup).
  std::string  provenance;           ///< The `"provenance"` field — where the row was materialized from.
  std::string  materializer_version; ///< The `"materializer_version"` field; empty when absent on the wire.
  std::string  current_materializer_version; ///< The `"current_materializer_version"` field; empty when absent.
  std::string  freshness = "current";        ///< The `"freshness"` field; defaults to `"current"` when absent on the wire.
};

/// @brief The `input` object of `planar task packet <id> --json`. Port
/// target: `state.zig`'s `TaskPacketInput`.
export struct task_packet_input {
  std::int64_t                 task_id = 0;         ///< The `"task_id"` field — must match the single dispatched
                                                    ///< task's id for `compile_brief`'s identity check to pass.
  std::string                  status;              ///< The `"status"` field — must match the dispatched task's own status.
  std::string                  title;               ///< The `"title"` field — rendered as the brief's authoritative task title.
  std::string                  body;                ///< The `"body"` field — rendered verbatim as the brief's task body.
  std::string                  next_action;         ///< The `"next_action"` field — rendered verbatim as the brief's
                                                    ///< "Exact next action".
  std::string                  acceptance_criteria; ///< The `"acceptance_criteria"` field — rendered as the
                                                    ///< brief's "Problem" section when the packet is authoritative.
  std::vector<packet_evidence> owning_plans;        ///< The `"owning_plans"` array; must contain exactly one row whose
                                                    ///< id matches the dispatched plan for the identity check to pass.
  std::vector<packet_evidence> anchor_plans;        ///< The `"anchor_plans"` array — feature-anchor plan evidence rows.
  std::vector<packet_evidence> citations;           ///< The `"citations"` array — spec-citation evidence rows.
  std::vector<packet_evidence> decisions;           ///< The `"decisions"` array — locked-decision evidence rows.
  std::vector<packet_evidence> questions;           ///< The `"questions"` array — open-question evidence rows.
  std::vector<packet_evidence> scenarios;           ///< The `"scenarios"` array — test-scenario coverage rows.
  std::vector<packet_evidence> dependencies;        ///< The `"dependencies"` array — task-dependency evidence rows.
  std::vector<packet_evidence> touches;             ///< The `"touches"` array — touched-surface evidence rows.
  std::vector<packet_evidence> claims;              ///< The `"claims"` array; `compile_brief` requires a row with
                                                    ///< `status == "active"` matching the caller's claim token.
  std::vector<packet_evidence> validation_gates;    ///< The `"validation_gates"` array — rendered as the brief's
                                                    ///< named gates.
  std::vector<packet_evidence> facts;               ///< The `"facts"` array — materialized-fact evidence rows.
};

/// @brief The whole authoritative packet document. Port target:
/// `state.zig`'s `TaskPacket`.
export struct task_packet {
  task_packet_input        input;   ///< The `"input"` object — the dispatched task's full authoritative content.
  std::string              digest;  ///< The `"digest"` field — the packet's content digest, rendered into the
                                    ///< brief header so a reviewer can confirm the coder saw this exact packet.
  std::vector<std::string> reasons; ///< The `"reasons"` array — why the packet is not ready; empty means ready
                                    ///< (see `ready()` below).

  /// @brief Whether the packet is ready to compile a brief from.
  /// @return `true` when `reasons` is empty, matching `TaskPacket.ready()`.
  [[nodiscard]] auto ready() const -> bool {
    return reasons.empty();
  }
};

/// @brief Parse `planar plan show <id> --json`'s stdout.
/// @param json The captured stdout.
/// @return The decoded value, or `state_parse_error::malformed`.
export auto parse_plan_show(std::string_view json) -> std::expected<plan_show, state_parse_error>;

/// @brief Parse `planar task show <id> --json`'s stdout.
/// @param json The captured stdout.
/// @return The decoded value, or `state_parse_error::malformed`.
export auto parse_task_show(std::string_view json) -> std::expected<task_show, state_parse_error>;

/// @brief Parse `planar task packet <id> --json`'s stdout.
/// @param json The captured stdout.
/// @return The decoded value, or `state_parse_error::malformed`.
export auto parse_task_packet(std::string_view json) -> std::expected<task_packet, state_parse_error>;

} // namespace state

// ---------------------------------------------------------------------------
// schema — CLI schema ingestion (port target: schema.zig)
// ---------------------------------------------------------------------------

/// @brief Pure JSON-decode helpers over `<bin> schema`'s flat catalog.
///
/// Port target: `zig/src/cmd/planar-execute/schema.zig`. As with `state`
/// above, the shelling is `host.cpp`'s job (`run_allowlisted(hs,
/// "planar-agent", {"schema"})`); this namespace is the pure parse half.
namespace schema {

/// @brief Why a `schema::parse_raw_schema` call failed. See
/// `state::state_parse_error` for the one-enumerator reasoning.
export enum class schema_parse_error : std::uint8_t {
  malformed,
};

/// @brief One flag on a command. Port target: `schema.zig`'s `FlagEntry`.
///
/// The Zig field name `long` is a C++ keyword; renamed `long_name` here —
/// the JSON wire key stays `"long"` (see `parse_raw_schema`'s
/// implementation).
export struct flag_entry {
  std::string                long_name;        ///< The JSON `"long"` key's value (renamed — `long` is a C++ keyword).
  std::vector<std::string>   aliases;          ///< The `"aliases"` array; empty when the flag has no aliases.
  std::optional<std::string> short_name;       ///< The `"short"` field (e.g. `"v"` for `-v`); unset when the flag
                                               ///< has no short form or the wire value was not a string.
  bool                       required = false; ///< The `"required"` field; `false` when absent on the wire.
  std::string                description;      ///< The `"description"` field; empty when absent on the wire.
};

/// @brief One command entry. Port target: `schema.zig`'s `CommandEntry`.
export struct command_entry {
  std::string              name;           ///< The `"name"` field — the command's short name.
  std::string              command;        ///< The `"command"` field — the full invocation path, e.g.
                                           ///< `"planar-agent complete"`; matched by `bin_schema::find_command`.
  std::vector<std::string> subcommands;    ///< The `"subcommands"` array; empty when this is a leaf command.
  std::vector<flag_entry>  flags;          ///< The `"flags"` array; empty when the command takes no flags.
  bool                     hidden = false; ///< The `"hidden"` field; `false` when absent on the wire. A hidden
                                           ///< command is skipped when `compile_brief` renders available verbs.
};

/// @brief The top-level `<bin> schema` document. Port target: `schema.zig`'s
/// `RawSchema`.
export struct raw_schema {
  std::uint32_t              schema_version = 0; ///< The `"schemaVersion"` field.
  std::string                layout;             ///< The `"layout"` field — the catalog's flattening shape.
  std::string                root;               ///< The `"root"` field — the binary's root command name,
                                                 ///< e.g. `"planar-agent"`; commands equal to this are skipped
                                                 ///< when `compile_brief` renders available verbs.
  std::vector<command_entry> commands;           ///< The `"commands"` array — the flat command list.
};

/// @brief A queryable wrapper over a parsed `raw_schema`. Port target:
/// `schema.zig`'s `BinSchema`.
export class bin_schema {
public:
  bin_schema() = default;

  /// @brief Wrap a decoded `raw_schema`.
  /// @param raw The decoded document. Copied in (the C++ tree has no
  /// arena-borrow lifetime to preserve — `RawSchema`'s Zig doc's "borrows
  /// from the Parsed arena" caveat does not apply here).
  explicit bin_schema(raw_schema raw) : _raw(std::move(raw)) {
  }

  /// @return The binary root name (e.g. `"planar-agent"`).
  [[nodiscard]] auto root() const -> std::string const& {
    return _raw.root;
  }

  /// @return The schema format version.
  [[nodiscard]] auto schema_version() const -> std::uint32_t {
    return _raw.schema_version;
  }

  /// @return The full flat command list.
  [[nodiscard]] auto commands() const -> std::vector<command_entry> const& {
    return _raw.commands;
  }

  /// @brief Look up a command by its full path string.
  /// @param full_path e.g. `"planar-agent complete"`.
  /// @return A pointer into `commands()`, or `nullptr` when absent.
  [[nodiscard]] auto find_command(std::string_view full_path) const -> command_entry const* {
    for (auto const& cmd : _raw.commands) {
      if (cmd.command == full_path) {
        return &cmd;
      }
    }
    return nullptr;
  }

private:
  raw_schema _raw;
};

/// @brief Parse `<bin> schema`'s stdout.
/// @param json The captured stdout.
/// @return The decoded value, or `schema_parse_error::malformed`.
export auto parse_raw_schema(std::string_view json) -> std::expected<raw_schema, schema_parse_error>;

} // namespace schema

// ---------------------------------------------------------------------------
// brief — the pure coder-brief compiler (port target: brief.zig)
// ---------------------------------------------------------------------------

/// @brief The pure, deterministic coder-brief compiler behind `ctx.brief`.
///
/// Port target: `zig/src/cmd/planar-execute/brief.zig`. `compile_brief`
/// performs no subprocess call, no filesystem I/O and no DB access; every
/// input is pre-gathered by the caller (`host.cpp`'s `host_ctx_brief`).
namespace brief {

/// @brief A single verbatim spec-section citation. Port target: `brief.zig`'s
/// `SpecCitation`.
export struct spec_citation {
  std::string                path;           ///< The spec section path, rendered as a "read firsthand" bullet.
  std::optional<std::string> verbatim_slice; ///< A verbatim excerpt to block-quote under `path`; unset when the
                                             ///< caller wants the coder to open the file rather than read a slice.
};

/// @brief A decision the coder must follow without re-litigating. Port
/// target: `brief.zig`'s `LockedDecision`.
export struct locked_decision {
  std::string id;   ///< The decision's identifier, rendered bold ahead of `text`.
  std::string text; ///< The decision's text, rendered verbatim.
};

/// @brief A single prior-stage context record. Port target: `brief.zig`'s
/// `ContextRef`.
export struct context_ref {
  std::string kind; ///< The record's kind (e.g. `"decision"`, `"handoff"`), rendered bold ahead of `body`.
  std::string body; ///< The record's rendered body text.
};

/// @brief All inputs to `compile_brief`. Port target: `brief.zig`'s
/// `BriefInputs`.
export struct brief_inputs {
  /// @brief When present, all implementation context is rendered from this
  /// authoritative packet. The live `ctx.brief` caller always sets it.
  std::optional<state::task_packet> authoritative_packet;

  state::plan_show               plan;  ///< The dispatched plan; identity-checked against
                                        ///< `authoritative_packet->input.owning_plans` when set.
  std::vector<state::task_entry> tasks; ///< The dispatched task(s); with an authoritative packet this must be
                                        ///< exactly one entry, identity-checked against the packet's `input`.

  std::string claim_token;       ///< The live claim token; rendered as the brief's claim token, and
                                 ///< identity-checked against `authoritative_packet->input.claims` when set.
  std::string problem_statement; ///< The freeform problem statement, rendered as the brief's "Problem" section
                                 ///< when no authoritative packet is set.

  std::vector<spec_citation>   spec_citations;   ///< Spec citations to render when no authoritative packet is set.
  std::vector<locked_decision> locked_decisions; ///< Locked decisions to render when no authoritative packet is set.

  schema::bin_schema agent_schema; ///< The parsed `planar-agent schema` catalog, used to render the brief's
                                   ///< "Available verbs" section.

  std::vector<std::string> gates; ///< Named validation gates to render when no authoritative packet is set.

  std::optional<std::string> context_capsule; ///< A compiled prior-stage capsule; unset or empty means no
                                              ///< capsule to render.
  std::vector<context_ref>   context_records; ///< Prior-stage context records; empty means none to render.
};

/// @brief Why `compile_brief` refused to compile.
export enum class brief_error : std::uint8_t {
  /// @brief The caller's `plan` / `tasks` / `claim_token` do not match the
  /// authoritative packet's identity. Port target: `brief.zig`'s
  /// `error.AuthoritativeIdentityMismatch`.
  authoritative_identity_mismatch,
};

/// @brief Assemble a methodology-compliant coder brief from `inputs`.
/// @param inputs The pre-gathered inputs.
/// @return The full brief text, or `brief_error::authoritative_identity_mismatch`.
export auto compile_brief(brief_inputs const& inputs) -> std::expected<std::string, brief_error>;

} // namespace brief

// ---------------------------------------------------------------------------
// Running a workflow
// ---------------------------------------------------------------------------

/// @brief Everything one `planar-execute run` invocation needs.
export struct run_config {
  std::string  workflow_name; ///< The workflow path, for diagnostics only.
  std::string  source;        ///< The workflow's bytes, already read.
  std::string  phase;         ///< The phase function to call.
  std::string  args_json;     ///< `--args` payload; empty means `{}`.
  std::string  worktree;      ///< `--worktree`; empty disables `git.*`.
  std::string  sandbox_root;  ///< `--sandbox-root`; empty disables `fs.*`.
  std::string  bin_dir;       ///< Directory holding the trusted sibling binaries.
  std::int64_t now  = 0;      ///< `ctx.now`.
  std::int64_t seed = 0;      ///< `ctx.seed`.
};

/// @brief How a run ended. Every non-`ok` value is exit 1 at the binary —
/// the oracle maps everything except `BadUsage` (which `run_workflow` cannot
/// produce) to 1, so this enum deliberately does not pretend to carry more
/// exit-code resolution than the oracle has.
export enum class run_status : std::uint8_t {
  ok,            ///< The phase ran; the payload (or `{}`) is on stdout.
  init_failed,   ///< The Lua state could not be created.
  load_failed,   ///< The chunk failed to compile, its top level errored, or `--args` did not decode.
  phase_missing, ///< No global of that name, or it is not a function.
  phase_failed,  ///< The phase raised, or called `flow.fail`.
};

/// @brief Load a workflow into a fresh sandbox, install the host surface,
/// call one phase, and marshal its `flow.result` payload.
///
/// Writes the JSON payload (or `{}`) plus a newline to `out` on success, and
/// every diagnostic — including `flow.log` lines DURING the run — to `err`.
/// stdout stays a clean JSON channel; that separation is the oracle's and is
/// pinned by the parity tests.
/// @param config The run configuration.
/// @param out The stdout stream.
/// @param err The stderr stream.
/// @return How the run ended.
export auto run_workflow(run_config const& config, std::ostream& out, std::ostream& err) -> run_status;

// ---------------------------------------------------------------------------
// Live introspection — the frozen-manifest and sandbox locks
// ---------------------------------------------------------------------------

/// @brief One name/type pair observed on a live sandboxed state.
export struct value_entry {
  std::string name; ///< The key.
  std::string type; ///< Lua's own type name: function, table, number, string, …
};

/// @brief Build a sandboxed state with the host surface installed, then
/// enumerate every FUNCTION reachable on the five host tables.
///
/// This is the frozen-manifest lock's left-hand side. It observes the state a
/// workflow would actually see rather than re-reading the manifest, which is
/// the entire point: a registration added outside the manifest-driven loop
/// shows up here and nowhere else.
/// @return The live `(table, name)` pairs, sorted.
export auto registered_host_surface() -> std::vector<std::pair<std::string, std::string>>;

/// @brief Enumerate `_G` on a live sandboxed state with the host surface
/// installed.
/// @return Every global's name and Lua type, sorted by name.
export auto sandbox_globals() -> std::vector<value_entry>;

/// @brief Enumerate one table on a live sandboxed state.
/// @param table The global table's name (e.g. `math`, `ctx`).
/// @return Its entries, sorted by name; empty when the global is absent or
/// is not a table.
export auto sandbox_table_entries(std::string_view table) -> std::vector<value_entry>;

/// @brief The `_VERSION` string of the linked Lua.
///
/// Pinned by a test because the sandbox's observable surface is
/// version-sensitive (5.5 adds `table.create`; its `math` carries
/// `acos`/`frexp`/`ldexp`, a 5.4 build's does not), so a silent Lua bump
/// would otherwise change what the sandbox enumeration sees while every host
/// function still appeared to work.
/// @return e.g. `"Lua 5.5"`.
export auto lua_version() -> std::string;

} // namespace planar::engine::execute
