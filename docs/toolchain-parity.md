# C++ toolchain parity (plan 996, D10)

## Status

M0 pin, task `cpp-toolchain-pin` (plan 996, C++26 rewrite). Recorded per D10
("latest LLVM (23+) is the toolchain; Planar derives its own pin and
import-std flags") and D4 (CMake ≥ 4.3 + `import std`). Adopts the pin-and-
verify method from `tabula`'s `README.md` § Pinned toolchain and
`centurion`'s `docs/toolchain-parity.md` — not their LLVM value. Every
finding below was verified empirically on this machine; the verification
scripts are committed at `scripts/toolchain-probes/`.

## Pinned versions (macOS, Homebrew, Apple Silicon)

| Component | Pinned version | Notes |
|---|---|---|
| LLVM/clang | **22.1.8** (Homebrew formula `llvm`, keg-only, `/opt/homebrew/opt/llvm`) | See "Why 22, not 23" below. |
| CMake | **4.4.2** | `import std;`'s experimental gate is a UUID keyed to the CMake feature release; verified against the actually-installed 4.4.2 (see below). |
| Ninja | **1.13.2** | Required for C++ module dependency scanning (dyndep); no other CMake generator supports it. |
| git | **2.50.1** (floor: **≥ 2.31**) | The floor was discovered in the M0 zig/-relocation cycle (commit `6423d9b`): `detectWorktree`'s git-common-dir fallback needs `git rev-parse --git-common-dir --path-format=absolute`, a flag introduced in git 2.31. Below that floor the path resolves incorrectly against the wrong base and a primary checkout gets misclassified as a secondary worktree. |
| Doxygen | not yet pinned by this task | Deferred to the milestone that turns on the `doxygen Doxyfile.lint` gate (tech-spec § Toolchain and conventions); tabula pins 1.17.0 for its `.cppm`-parsing quality — reuse that value when the gate lands unless a newer verified release exists then. |

### Why 22, not 23

D10 targets "latest LLVM (23+)". Verified against the upstream release feed
(`https://api.github.com/repos/llvm/llvm-project/releases`) on 2026-08-21:
the newest tag is `llvmorg-23.1.0-rc3` — LLVM 23 is still a release
candidate, not a stable release, and Homebrew's `llvm` formula (which always
tracks the latest **stable** major) is at `22.1.8`, identical to the
`llvm@22` versioned formula. There is no installable LLVM 23 on this
platform today. The pin is therefore **LLVM 22.1.8**, the actual latest
stable major, matching (coincidentally — each project derives its own pin
per D10) both `tabula`'s and `centurion`'s current pins. **Re-pin to LLVM 23
as soon as it reaches a stable release and Homebrew ships it**; re-run the
probes in `scripts/toolchain-probes/` against the new toolchain before
updating this table.

## Platform prefixes

| Platform | Compiler prefix | Status |
|---|---|---|
| macOS (Homebrew, Apple Silicon) | `/opt/homebrew/opt/llvm` (keg-only; not on `PATH`) | **Verified** — this document's probes ran against it directly. |
| macOS (Homebrew, Intel) | `/usr/local/opt/llvm` | Not verified on this task (no Intel Mac available); same keg-only layout, different prefix root — Homebrew's own convention. |
| Linux (apt.llvm.org, Debian/Ubuntu) | `/usr/lib/llvm-<N>/bin/clang++`, alternatives symlink `clang++-<N>` at `/usr/bin/clang++-<N>` | **Specified, not yet verified.** Derived from centurion's `docs/toolchain-parity.md` (its `linux-container-base` preset pins `/usr/bin/clang-22` / `/usr/bin/clang++-22` against `llvm-toolchain-<codename>-22`, matching apt.llvm.org's major-and-codename-pinned repository naming). Planar's own Linux lane does not exist yet (tech-spec § Presets: `linux-container` "follows post-cutover"); when it lands, pin apt.llvm.org's `llvm-toolchain-<codename>-<major>` for whatever major this table currently pins (23 once stable, else the same major as the macOS row) and verify with the same probes under the target Debian/Ubuntu base image before trusting this row. |

## Derived import-std / embed flag set (macOS, verified)

Configure-time flags, mirroring the pattern in `centurion/CMakeLists.txt`
and `centurion/CMakePresets.json`'s `base` preset (Homebrew LLVM is
keg-only, so nothing here is discoverable from a bare `clang++` on `PATH`):

```
CMAKE_EXPERIMENTAL_CXX_IMPORT_STD = f35a9ac6-8463-4d38-8eec-5d6008153e7d   # CMake 4.4.x gate UUID
CMAKE_C_COMPILER                 = /opt/homebrew/opt/llvm/bin/clang
CMAKE_CXX_COMPILER               = /opt/homebrew/opt/llvm/bin/clang++
CMAKE_CXX_FLAGS                  = -stdlib=libc++ -nostdinc++ -isystem /opt/homebrew/opt/llvm/include/c++/v1
CMAKE_EXE_LINKER_FLAGS           = -stdlib=libc++ -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++
CMAKE_CXX_STDLIB_MODULES_JSON    = /opt/homebrew/opt/llvm/lib/c++/libc++.modules.json
```

Per target (not global — forcing it onto vendored CPM targets like Glaze or
libcurl breaks their own configure):

```
CXX_MODULE_STD = ON          # target property
cxx_std_26                   # target_compile_features
```

`-nostdinc++ -isystem <llvm>/include/c++/v1` is required on macOS because the
Xcode Command Line Tools SDK ships its own libc++ headers; without forcing
the Homebrew path first, the compiler silently picks up the SDK's mismatched
libc++ instead of the pinned one — this is the same reasoning as
`tabula`'s and `centurion`'s macOS presets, re-verified here rather than
assumed.

**Verification** (`scripts/toolchain-probes/`, full transcript in the
validation table below): a scratch CMake 4.4.2 + Ninja 1.13.2 project using
exactly the flag set above configured clean (exit 0, only the expected
"experimental" CMake warning) and built `import std;` (`std::vector`,
`std::ranges::sort`, `std::println`) plus `std::expected` end to end —
compiled, linked against libc++, and ran with correct output.

## `-std=c++2c` and the C++26 subset

`-std=c++2c -stdlib=libc++` was accepted with no diagnostics for a probe
using **pack indexing** (`vals...[0]`, P2662R3, C++26-only — not available
under `-std=c++23`), confirming the compiler is actually parsing C++26 mode,
not silently falling back.

`#embed` (D5's mechanism for embedding `migrations/` and
`templates/defaults/`) compiles and runs correctly under `-std=c++2c`, but
clang 22.1.8 emits `-Wc23-extensions` ("#embed is a Clang extension") in
C++ mode — it has not yet reclassified `#embed` as core C++26 syntax
internally, even though WG21 adopted `#embed` for C++26 (P1967). Under the
tech-spec's "warnings-as-errors on first-party targets" convention this
diagnostic becomes a build failure unless suppressed. **Any first-party
target that uses `#embed` needs `-Wno-c23-extensions`** (verified: adding
it alongside `-Werror` restores a clean, zero-diagnostic build). Scope the
flag to the specific `db`/`config` embed-generator targets described in the
tech-spec's "Embedded migrations and templates" section, not globally —
re-check this warning's classification whenever the pinned LLVM major moves.

`std::expected` is usable (`#include <expected>` or via `import std;`,
verified above) — no separate feature gate needed beyond libc++'s existing
C++23 support, which C++26 mode inherits.

## clang-format binary path rule (D16)

Per D16 (tabula's format/tidy configs, latest-LLVM dialect) and the
tech-spec's "The pinned LLVM's clang-format is the only formatter whose
output counts": all formatting and lint tooling must invoke the **pinned
toolchain's own binary**, never a `PATH`-resolved `clang-format` that may
belong to a different LLVM release (two LLVM releases can disagree under an
identical `.clang-format`, per tabula's README). On this machine that is
`/opt/homebrew/opt/llvm/bin/clang-format`, verified at
`Homebrew clang-format version 22.1.8` — matching the pinned compiler major
and patch exactly, as it must (same Homebrew keg). CMake tooling and any
pre-commit/format-check script must reference this path explicitly (e.g. via
`CMAKE_CXX_COMPILER`-relative derivation, the same pattern the
`CMAKE_CXX_STDLIB_MODULES_JSON` discovery in `tabula`'s top-level
`CMakeLists.txt` uses), not a bare `clang-format` invocation.

## Validation evidence

All commands below ran in the foreground on this machine (macOS, Apple
Silicon, Homebrew) on 2026-08-21. Probe sources are committed at
`scripts/toolchain-probes/probe_cxx2c.cpp` and
`scripts/toolchain-probes/probe_embed.cpp` (plus its embedded fixture
`probe_embed_data.txt`); the `import std;` probe is a throwaway scratch
CMake project (not committed — its shape is fully specified by the flag set
above and centurion's `CMakeLists.txt`/`CMakePresets.json` pattern it
mirrors) built under `cmake -S . -B build -G Ninja` with the exact
`CMAKE_CXX_FLAGS`/`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_CXX_STDLIB_MODULES_JSON`
values listed above.

| Probe | Command | Exit | Evidence |
|---|---|---|---|
| `-std=c++2c` + pack indexing | `clang++ -std=c++2c -stdlib=libc++ -Wall -Wextra scripts/toolchain-probes/probe_cxx2c.cpp -o /tmp/probe_cxx2c && /tmp/probe_cxx2c` | 0 | `pack indexing ok: 7`, no diagnostics |
| `#embed` | `clang++ -std=c++2c -stdlib=libc++ -Wall -Wextra scripts/toolchain-probes/probe_embed.cpp -o /tmp/probe_embed && /tmp/probe_embed` | 0 | `embedded 25 bytes: planar-cpp26-embed-probe`; one `-Wc23-extensions` warning (see above) |
| `#embed` under `-Werror` | `clang++ -std=c++2c -stdlib=libc++ -Wall -Wextra -Werror scripts/toolchain-probes/probe_embed.cpp -o /tmp/probe_embed_werror` | 1 | `error: #embed is a Clang extension [-Werror,-Wc23-extensions]` — confirms the suppression is load-bearing |
| `#embed` under `-Werror -Wno-c23-extensions` | same, plus `-Wno-c23-extensions` | 0 | clean build, correct output — confirms the fix |
| `import std;` configure | `cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=... -DCMAKE_CXX_COMPILER=.../clang++ -DCMAKE_CXX_FLAGS="-stdlib=libc++ -nostdinc++ -isystem .../include/c++/v1" -DCMAKE_EXE_LINKER_FLAGS="-stdlib=libc++ -L.../lib/c++ -Wl,-rpath,.../lib/c++" -DCMAKE_CXX_STDLIB_MODULES_JSON=.../lib/c++/libc++.modules.json` | 0 | Only the expected CMake "experimental `import std;`" author warning |
| `import std;` build + run | `cmake --build build && ./build/probe_import_std` | 0 | `sorted: 1 2 3` / `expected ok` — `std::vector`, `std::ranges::sort`, `std::println`, and `std::expected` all resolve and link through libc++'s std module |

No C++26 feature this task needed to verify failed outright; the only
caveat is the `#embed` extension-warning interaction with warnings-as-errors,
documented above with its exact fix.
