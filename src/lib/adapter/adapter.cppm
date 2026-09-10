/// @file adapter.cppm
/// @brief `planar.adapter` — the external-plane adapter boundary: the value
/// types a Jira/GitHub adapter exchanges with the sync engine, and the
/// interface it implements (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of the non-transport half of
/// `zig/src/engine/extsync/common.zig` — `AuthKind`, `AuthCredential`,
/// `RemoteState`, `LocalEntity`, `CreateOptions`, `CreatedEntity`,
/// `FieldChangeSet`, `UpdateOutcome`, and the `dispatch`/`requireAdapter`
/// comptime interface check.
///
/// ## Why this is layer 1 and not part of the adapter bucket
///
/// It is the ONE thing `engine_extsync` (which implements adapters) and
/// `engine_external` (whose sync engine drives one) both need, and D18
/// forbids the edge between them. So it moves down a layer, exactly as
/// `scope_ref` and `json_text` did (D19). Note the direction this makes
/// legal: `engine_external::sync` takes an `adapter&` and never names Jira or
/// GitHub at all, so the sync engine can be unit-tested against a stub
/// adapter with no HTTP anywhere in the picture.
///
/// This module depends on nothing — not even `planar.http`. An adapter needs
/// a transport; the BOUNDARY does not.
///
/// ## The interface is virtual where the Zig original was comptime
///
/// `extsync.dispatch(Adapter, .pull, ...)` is a `@hasDecl` check over an
/// `anytype`: a compile-time structural interface with no runtime
/// representation. The C++ equivalent of "any type with these four
/// operations" that also erases the concrete type at a function boundary is
/// an abstract base class, and that is what this is. `pullLink` /
/// `pushLink` / `resolveConflict` all took `adapter: anytype` in Zig and all
/// take `const external_adapter&` here. The class is `external_adapter`
/// rather than `adapter` because `planar::adapter::adapter` would shadow its
/// own namespace inside every derived class scope — `adapter::remote_state`
/// then resolves to the BASE CLASS, not the namespace, and every use fails.
///
/// ## `adapter_error_name` is a PARITY SURFACE, not a debugging aid
///
/// `zig/src/engine/external/sync.zig`'s failure path writes the adapter
/// error into `sync_events.detail` as `@errorName(e)` — the Zig error tag
/// spelled exactly as declared. Those strings are therefore observable
/// through `planar audit trail --link <id> --json`, and every name this
/// function returns is the corresponding Zig tag verbatim, PascalCase and
/// all. Renaming one is a parity break, not a rename.
module;

export module planar.adapter;

import std;

namespace planar::adapter {

/// @brief How the adapter authenticates. Mirrors the Zig `AuthKind`.
export enum class auth_kind : std::uint8_t {
  bearer, ///< `Authorization: Bearer <token>`.
  basic,  ///< `Authorization: Basic base64(user:pass)`.
};

/// @brief The credential an adapter presents.
///
/// The fields are optional exactly as in the Zig original, and an adapter
/// that finds the field its `kind` requires unset returns
/// `adapter_error::invalid_auth` rather than sending an empty header.
export struct auth_credential {
  auth_kind                  kind = auth_kind::bearer; ///< Which scheme.
  std::optional<std::string> token;                    ///< The bearer token.
  std::optional<std::string> user;                     ///< The basic-auth user.
  std::optional<std::string> pass;                     ///< The basic-auth password.
};

/// @brief The remote ticket's state, normalized across providers.
///
/// `status` is the LOCAL vocabulary (`todo`/`doing`/`blocked`/`done`/
/// `cancelled`), already mapped by the adapter; `raw_status` is what the
/// provider actually said. Keeping both is load-bearing — the sync engine
/// compares `status`, and the operator-facing surfaces show `raw_status`.
export struct remote_state {
  std::string  external_id;  ///< The provider's id for the ticket.
  std::string  title;        ///< The ticket title.
  std::string  body;         ///< The ticket description, flattened to text.
  std::string  status;       ///< The ticket status in PLANAR's vocabulary; empty when unmappable.
  std::string  assignee;     ///< The assignee, empty when unassigned.
  std::int64_t priority = 0; ///< The provider's numeric priority, 0 when absent.
  std::string  due_at;       ///< The due date, empty when unset.
  std::string  url;          ///< The ticket URL, empty when the provider did not give one.
  std::string  raw_status;   ///< The provider's own status string.
  /// @brief A stable remote-side version marker — normally the provider's
  /// `updated_at`. Empty ONLY for a provider exposing no version metadata,
  /// and that emptiness is itself load-bearing: `resolve_conflict` refuses to
  /// authorize either side when it is empty. See
  /// `planar.engine.external.sync`.
  std::string version;
};

/// @brief The local entity being rendered into a provider payload.
export struct local_entity {
  std::string  kind;         ///< The local table (`task`, `plan`, ...).
  std::int64_t id = 0;       ///< The local row id.
  std::string  title;        ///< The local title.
  std::string  body;         ///< The local body.
  std::string  status;       ///< The local status.
  std::int64_t priority = 0; ///< The local priority.
  std::string  due_at;       ///< The local due date.
};

/// @brief Per-creation options a provider payload may carry.
export struct create_options {
  std::optional<std::string> issue_type; ///< The provider issue type; adapter-defaulted when unset.
  std::optional<std::string> project;    ///< The provider project/repo; omitted from the payload when unset.
};

/// @brief The subset of fields a push is asking the provider to change.
///
/// An unset field is "not requested", NOT "set to empty" — an adapter skips
/// it entirely rather than clearing the remote value.
export struct field_change_set {
  std::optional<std::string> title;    ///< The new title.
  std::optional<std::string> status;   ///< The new status, in Planar's vocabulary.
  std::optional<std::string> assignee; ///< The new assignee.
  std::optional<std::string> priority; ///< The new priority.
};

/// @brief What a push actually changed.
export struct update_outcome {
  /// @brief The field names the adapter applied, in the order it applied
  /// them. Written to `sync_events.fields_changed` as a JSON array.
  std::vector<std::string> fields_applied;
};

/// @brief Why an adapter operation failed.
///
/// The union of the Zig `jira.Error` and `github.Error` sets. Both declare
/// the same eight tags; see `adapter_error_name` for why the spelling of each
/// is a parity contract.
export enum class adapter_error : std::uint8_t {
  invalid_external_id, ///< The id does not match the provider's format.
  not_found,           ///< The provider returned 404.
  unexpected_status,   ///< The provider returned a status the operation does not accept.
  transport_failed,    ///< No response was obtained at all.
  parse_failed,        ///< The response body did not parse as the expected shape.
  encode_failed,       ///< The request body could not be built.
  invalid_auth,        ///< The credential is missing the field its scheme needs.
  write_failed,        ///< A write to the response buffer failed.
};

/// @brief The Zig error tag for `err`, as `sync_events.detail` records it.
///
/// See this module's header: these strings are observable through
/// `audit trail --link`, so they are PascalCase Zig tags, not C++ names.
/// @param err The error.
/// @return The tag.
export auto adapter_error_name(adapter_error err) -> std::string_view;

/// @brief The four operations every external-plane adapter implements.
///
/// The Zig original enforces this set with a `@compileError` per missing
/// declaration in `requireAdapter`; here the pure-virtual functions do it.
export class external_adapter {
public:
  external_adapter()                                   = default;
  external_adapter(const external_adapter&)            = delete;
  external_adapter& operator=(const external_adapter&) = delete;
  external_adapter(external_adapter&&)                 = delete;
  external_adapter& operator=(external_adapter&&)      = delete;
  virtual ~external_adapter();

  /// @brief Check that `external_id` is well-formed for this provider.
  ///
  /// Purely syntactic — it never contacts the provider. Every other
  /// operation calls it first.
  /// @param external_id The provider-side id.
  /// @return Success, or `adapter_error::invalid_external_id`.
  [[nodiscard]] virtual auto validate(std::string_view external_id) const -> std::expected<void, adapter_error> = 0;

  /// @brief Read the remote ticket's current state.
  /// @param external_id The provider-side id.
  /// @return The normalized state, or the failure.
  [[nodiscard]] virtual auto pull(std::string_view external_id) const -> std::expected<remote_state, adapter_error> = 0;

  /// @brief Apply `fields` to the remote ticket.
  ///
  /// A change set that names nothing this provider can update through this
  /// endpoint sends NO request at all and returns an empty outcome — see the
  /// Jira adapter, where a status-only push is exactly that case.
  /// @param external_id The provider-side id.
  /// @param fields The requested changes.
  /// @return What was applied, or the failure.
  [[nodiscard]] virtual auto push(std::string_view external_id, const field_change_set& fields) const
      -> std::expected<update_outcome, adapter_error> = 0;

  /// @brief Render a creation payload for `local`.
  ///
  /// Returns the provider's JSON body WITHOUT sending it — `ext create` is
  /// what sends it, and that verb is not ported yet (see
  /// engine/extsync/CMakeLists.txt). The renderer is here anyway because it
  /// is part of the four-operation interface the Zig original enforces, and
  /// because it is fully testable with no transport at all.
  /// @param local The local entity.
  /// @param opts The creation options.
  /// @return The JSON payload, or the failure.
  [[nodiscard]] virtual auto render(const local_entity& local, const create_options& opts) const
      -> std::expected<std::string, adapter_error> = 0;
};

} // namespace planar::adapter
