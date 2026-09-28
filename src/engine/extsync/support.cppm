/// @file support.cppm
/// @brief `planar.engine.extsync.support` — the two request-building
/// primitives the Jira and GitHub adapters both need (plan 996, task 6041).
///
/// Behavior-preserving port (D2) of `authHeader` and `trimTrailingSlash`,
/// which `zig/src/engine/extsync/jira.zig` and `github.zig` each declare
/// SEPARATELY and IDENTICALLY (jira.zig:190-209 / github.zig:860-884 —
/// byte-for-byte the same two functions in both files). They are stated once
/// here because both adapters live in the same target, so sharing them
/// creates no dependency edge of any kind.
///
/// This is NOT the D19 extract-to-layer-1 move: nothing outside this bucket
/// needs either function.
module;

export module planar.engine.extsync.support;

import std;
import planar.adapter;

namespace planar::engine::extsync::support {

/// @brief Build the `Authorization` header value for `cred`.
///
/// `bearer` renders `Bearer <token>`; `basic` renders
/// `Basic base64(user:pass)` with the STANDARD base64 alphabet and padding
/// (Zig's `std.base64.standard`), not the URL-safe one.
///
/// A credential missing the field its scheme requires is
/// `adapter_error::invalid_auth` — the Zig original returns `InvalidAuth`
/// rather than sending `Bearer ` with an empty token, and that distinction is
/// observable: an empty bearer reaches the provider and comes back 401
/// (`UnexpectedStatus`), while this returns before any request is sent.
/// @param cred The credential.
/// @return The header value, or `adapter_error::invalid_auth`.
export auto auth_header(const adapter::auth_credential& cred) -> std::expected<std::string, adapter::adapter_error>;

/// @brief Drop ONE trailing `/` from `s`, if present.
///
/// Exactly one, matching the Zig original — `https://host//` trims to
/// `https://host/`, not to `https://host`. Every URL an adapter builds
/// concatenates `base + "/rest/..."`, so this is what keeps a
/// configured-with-a-slash base URL from producing a double slash.
/// @param s The base URL.
/// @return The trimmed view, borrowing from `s`.
export auto trim_trailing_slash(std::string_view s) -> std::string_view;

} // namespace planar::engine::extsync::support
