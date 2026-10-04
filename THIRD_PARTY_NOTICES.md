# Third-party notices

Planar is licensed under the MIT license (see [LICENSE](LICENSE)). It includes
the third-party software below, cached unmodified under `vendor/` so a
configured build does not touch the network. Each package's own license text
travels with its source. Where a package ships no separate license file, the
notice is given here or in the source header noted.

| Package | License | Where the notice lives |
| --- | --- | --- |
| Catch2 | BSL-1.0 | `vendor/catch2/*/LICENSE.txt` |
| CLI11 | BSD-3-Clause | `vendor/cli11/*/LICENSE` |
| curl | curl license | `vendor/curl/*/COPYING` |
| Glaze | MIT | `vendor/glaze/*/LICENSE` |
| Lua | MIT | header of `vendor/lua/*/src/lua.h` (reproduced below) |
| spdlog | MIT | `vendor/spdlog/*/LICENSE` |
| SQLite | Public domain | none required (see below) |
| Tree-sitter | MIT | `vendor/tree_sitter/*/LICENSE` |
| tree-sitter-zig | MIT | `vendor/tree_sitter_zig/*/LICENSE` |

## Lua

Copyright (C) 1994-2025 Lua.org, PUC-Rio.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

## SQLite

The SQLite amalgamation is in the public domain. See
<https://sqlite.org/copyright.html>.
