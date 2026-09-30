/// @file textview.cppm
/// @brief `planar.textview` — the terminal-safe text primitives the host
/// queue's human-readable views share (plan 1080, task 7082).
///
/// A queue entry carries text its submitter chose: vendor, role, label,
/// directory and argument words. Printed raw, such a value can rewrite the
/// line it sits on (a bidirectional override, a line or paragraph separator, a
/// control byte), widen a column without limit, or be mistaken for the
/// framing around it (a leading double quote). This module holds the rules
/// that keep a value on its own cell, shared so the two binaries that print
/// them (`planar-watch` and `planar-agent`) cannot drift apart.
///
/// Every function is pure and infallible; nothing here allocates beyond its
/// result, opens a file or writes. JSON output does not use this module: it
/// stays complete and is escaped by its own renderers.
///
/// ## Display width
///
/// `display_width` is an approximation, not a full Unicode width table: it
/// counts UTF-8 code points, counts a combining mark or a format character as
/// 0, a wide East Asian or emoji code point as 2, and each byte of an invalid
/// sequence as 1. It exists so columns line up for the scripts a terminal
/// commonly shows; it does not promise agreement with every terminal.
module;

export module planar.textview;

import std;

namespace planar::textview {

/// @brief The display columns a vendor, role or label may take in a text
/// view before it is cut and marked with `…`.
export inline constexpr std::size_t k_field_cap = 48;

/// @brief The marker a cut value ends with.
export inline constexpr std::string_view k_truncation_marker = "\xE2\x80\xA6"; // U+2026

/// @brief Whether a code point is a Unicode format character a terminal must
/// not receive raw: the bidirectional controls U+202A-U+202E and
/// U+2066-U+2069, U+061C, the zero-width and directional marks U+200B-U+200F,
/// the line and paragraph separators U+2028 and U+2029, U+2060-U+2064,
/// U+206A-U+206F and the byte-order mark U+FEFF.
/// @param cp The code point.
/// @return True when it is one of those.
export auto is_format_char(char32_t cp) -> bool;

/// @brief Whether `text` holds anything that must not reach a terminal or a
/// line-oriented reader raw: a C0 control, DEL, a C1 control (U+0080-U+009F),
/// a format character (`is_format_char`) or a byte that is not valid UTF-8.
/// @param text The bytes to inspect.
/// @return True when one is present.
export auto has_hazard(std::string_view text) -> bool;

/// @brief `value` as a double-quoted string, escaping the quote, the
/// backslash, every control byte (C0, DEL, C1) and every format character
/// (`\n`, `\r` and `\t` in their short forms, the rest as `\u00xx` or
/// `\uxxxx`), and a byte that is not valid UTF-8 as `\xNN`. It is display
/// text, not JSON (`\xNN` is not a JSON escape): a caller that emits JSON does
/// not use it.
/// @param value The bytes to quote.
/// @return The quoted text.
export auto quote_text(std::string_view value) -> std::string;

/// @brief The approximate number of terminal columns `text` takes; see the
/// module header for what is approximated.
/// @param text UTF-8 text.
/// @return The column count.
export auto display_width(std::string_view text) -> std::size_t;

/// @brief `text` unchanged when it takes at most `max_width` columns,
/// otherwise its longest prefix that fits with `k_truncation_marker` after it
/// in `max_width` columns. A code point is never split.
/// @param text UTF-8 text.
/// @param max_width The most columns the result may take; at least 1.
/// @return The possibly cut text.
export auto truncate_display(std::string_view text, std::size_t max_width) -> std::string;

/// @brief `truncate_display(text, k_field_cap)`: the bound the text views put
/// on a vendor, role or label.
/// @param text The value as submitted.
/// @return The value as a text view shows it.
export auto cap_field(std::string_view text) -> std::string;

} // namespace planar::textview
