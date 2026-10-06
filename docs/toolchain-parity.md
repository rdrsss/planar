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
| LLVM/clang | **23.1.0** (Homebrew formula `llvm`, keg-only, `/opt/homebrew/opt/llvm`) | See "Why 23, and the 22 interim" below. |
| CMake | **4.4.2** | `import std;`'s experimental gate is a UUID keyed to the CMake feature release; verified against the actually-installed 4.4.2 (see below). |
| Ninja | **1.13.2** | Required for C++ module dependency scanning (dyndep); no other CMake generator supports it. |
| git | **2.50.1** (floor: **≥ 2.31**) | The floor was discovered in the M0 zig/-relocation cycle (commit `6423d9b`): `detectWorktree`'s git-common-dir fallback needs `git rev-parse --path-format=absolute --git-common-dir`, a flag introduced in git 2.31. `--path-format=absolute` must precede `--git-common-dir` (it is a mode flag that governs how the path options after it are printed). Below that floor the path resolves incorrectly against the wrong base and a primary checkout gets misclassified as a secondary worktree. |
| Doxygen | **1.18.0** (Homebrew formula `doxygen`) — not version-pinned, tracked as-installed | The `doxygen Doxyfile.lint` gate (`make cpp-lint`) is live (plan 996, task `cpp-lint-doxygen-sigbus`). 1.18.0 has a nondeterministic upstream defect: it dies with SIGBUS (exit 138) on a clean, unchanged tree, unrelated to load, config, or content. Measured by the orchestrator on 2026-08-22: 6 standalone runs against an idle machine produced exits `0,138,0,0,138,0` — roughly a **25-50% crash rate**. `scripts/cpp-lint-doxygen.sh` retries doxygen up to 3 attempts total, but ONLY when the captured output of the attempt contains no WARN_FORMAT diagnostic line — a genuine finding is authoritative the moment it is printed, regardless of whether doxygen then also dies from a signal while unwinding (task 6315; see "Doxygen retry loop discriminates on captured output, not just exit code" below). Each retry prints a visible message so a rising crash rate remains observable. Re-evaluate this row (and reconsider pinning a fixed Doxygen release) the next time Doxygen is upgraded — compare the new version's crash rate against this 25-50% baseline before assuming the upstream bug is fixed. 1.18.0 also mangles the `override` substring inside identifiers used as template arguments in a trailing return type — see "Doxygen 1.18.0 mangles `override` inside identifiers" below (task 6323). |

### Why 23, and the 22 interim

D10 targets "latest LLVM (23+)". As of 2026-08-21, LLVM 23 was still a
release candidate (`llvmorg-23.1.0-rc3` against the upstream release feed)
and Homebrew's `llvm` formula tracked `22.1.8` — the pin was recorded as
**LLVM 22.1.8** at that time, matching (coincidentally — each project
derives its own pin per D10) both `tabula`'s and `centurion`'s pins of the
day.

Re-verified 2026-09-04: Homebrew's `llvm` formula (aliased `llvm@23`) now
ships **23.1.0 stable** (`brew info llvm` reports `stable 23.1.0
(bottled)`, no `HEAD`/RC qualifier), and `/opt/homebrew/opt/llvm` resolves
to it. The pin moves to **LLVM 23.1.0** accordingly — this is the first
stable major matching D10's "latest LLVM (23+)" target. `CMakePresets.json`
then pinned the compiler by **path**, not by version string
(`/opt/homebrew/opt/llvm/bin/clang++`; it now names a toolchain file, see
"How the prefix is found" below), so the keg swap from 22.1.8 to
23.1.0 required no preset change; the debug/release builds configure and
build clean against it. Every probe in `scripts/toolchain-probes/` was
re-run against 23.1.0 and reproduced identical behavior to the 22.1.8
findings recorded below, including the `-Wc23-extensions` diagnostic on
`#embed` — LLVM has not yet reclassified `#embed` as core C++26 syntax in
23.1.0 either, so the `-Wno-c23-extensions` suppression documented under
"`-std=c++2c` and the C++26 subset" remains necessary unchanged.
`llvm@22` (22.1.8) is still installed alongside it on this machine but is
no longer the pin; re-run the probes again before trusting a future major
bump.

### Doxygen retry loop discriminates on captured output, not just exit code

Task 6315. `scripts/cpp-lint-doxygen.sh` originally split behavior purely on
the shell exit-status family: `>= 128` (terminated by signal) meant "known
SIGBUS flake, retry"; anything in `[1,127]` meant "genuine `WARN_AS_ERROR`
finding, fail immediately". That split is necessary but was not sufficient:
task 6303 and task 6330 each found a genuine doc-comment defect (undocumented
params/members) that got absorbed by the retry loop and reported as flake
noise instead.

The reason: `Doxyfile.lint` sets `WARN_AS_ERROR = YES`, whose documented
behavior is to "immediately stop when a warning is encountered" — and on this
1.18.0 build that stop can itself die from a signal instead of a clean
`exit(1)`, *after* the diagnostic line has already been written to stderr per
`WARN_FORMAT`. A crash in that family is indistinguishable from the
content-independent SIGBUS flake by exit code alone.

The fix: the script now captures every attempt's combined stdout+stderr and
scans it for a line matching `WARN_FORMAT`'s shape
(`^[^[:space:]]+:[0-9]+: `, i.e. `$file:$line: $text`) *before* looking at the
exit code. Since `QUIET = YES` means that pattern is the only kind of line
doxygen ever writes on a clean run, any match is a genuine diagnostic and is
now authoritative — printed and failed immediately, with no retry — even if
the process then also dies from a signal. Only a crash whose output contains
no such line is treated as the flake and retried, up to the existing 3-attempt
bound.

Verified with two proofs (both read the script's own exit code directly, not
through a pipe):

- A real `WARN_AS_ERROR` finding (a `.cppm` with an undocumented parameter,
  clean `exit 1`, no signal) fails on attempt 1/3 with no retry message.
- A stub `doxygen` that dies from `SIGBUS` with no diagnostic output on its
  first invocation and exits 0 on its second reproduces the original retry
  path unchanged: one retry message, then a clean pass.
- A stub `doxygen` that prints a `WARN_FORMAT`-shaped diagnostic line and
  *then* dies from `SIGBUS` on every attempt — the exact task-6315 masking
  shape — now fails immediately on attempt 1/3 with the diagnostic reprinted,
  instead of retrying and possibly reporting a false pass on a later attempt.

### Doxygen 1.18.0 mangles `override` inside identifiers

Task 6323. Reproduced empirically: doxygen 1.18.0 tokenizes the `override`
**keyword** out of the middle of an identifier when that identifier is used
as a template argument **inside a trailing return type** (`-> std::expected<
some_override_thing, other_type>`, etc.). It does not misfire when the same
identifier appears as a function parameter, a struct/class member's
declared type, an enum value, or a template argument in an ordinary
(non-trailing-return-type) declaration — all of those parse clean. The
narrower a repro gets, the more this looks like the trailing-`->` parse path
specifically mis-tokenizing its argument list, not a blanket "the word
override is special" bug.

Minimal repro (fully documented on both sides, to make the false positive
unambiguous):

```cpp
export enum class decode_error : std::uint8_t { syntax };
export struct manual_overrides { std::int64_t schema_version = 0; };
export auto load(const std::filesystem::path& path)
    -> std::expected<manual_overrides, decode_error>;
```

fails with:

```
error: Member decode_error (variable) of namespace probe is not documented.
```

— `decode_error` **is** documented; doxygen has misparsed
`std::expected<manual_overrides, decode_error>`, apparently reading
`manual_` + `override` + `s` as the keyword `override` embedded in the first
template argument, which knocks the rest of the trailing-return-type parse
off the rails and makes it treat the second template argument as an
undocumented namespace-scope variable declaration instead. Renaming
`manual_overrides` to `manual_edits` (no `override` substring, same
semantics) makes the identical declaration parse clean with zero warnings.

This exact defect and repro were hit for real in
`src/engine/workspace/routing.cppm`'s `manual_edits` type (see the
`## Why this type is NOT called \`overrides\`` comment on that struct for the
full investigation, including two wrong hypotheses ruled out along the way);
that is the only occurrence in the current tree, and it already carries the
rename fix. A repo-wide sweep for `override`-substring identifiers used as
template arguments (`grep -rnE '<[^<>]*[a-zA-Z_]override[a-zA-Z_]*[^<>]*>'`)
found no other live occurrence.

**Chosen fix: none of `Doxyfile.lint`, a suppression, or a rename was needed
this cycle** (the one prior occurrence is already renamed) — this section
exists so the trigger is documented at the canonical toolchain-parity
location instead of only inside one struct's comment, and so a future author
does not re-spend the investigation. **Rejected approaches**, in order of how
much they would have weakened the gate: (1) loosening `WARN_AS_ERROR` off
`YES` — rejected outright, it would silence every future genuine
undocumented-export finding tree-wide, not just this false positive; (2) a
blanket per-file Doxygen suppression — rejected, `Doxyfile.lint` has no
per-line escape hatch analogous to `cli-lint-ignore`, and a file-wide
suppression would hide real findings elsewhere in the same file; (3) a
`Doxyfile.lint` setting — none exists that disables keyword-substring
tokenization for identifiers. Renaming the offending identifier is the
narrowest fix that keeps `WARN_AS_ERROR = YES` fully in force everywhere
else, and is what actually resolved the one occurrence found. If a future
`.cppm`/`.cpp` hits this again: rename the identifier so it does not contain
`override` as a substring (matching case, e.g. `Override`/`OVERRIDE` too) —
do not reach for a suppression.

## Platform prefixes

| Platform | Compiler prefix | Status |
|---|---|---|
| macOS (Homebrew, Apple Silicon) | `/opt/homebrew/opt/llvm` (keg-only; not on `PATH`) | **Verified** — this document's probes ran against it directly. |
| macOS (Homebrew, Intel) | `/usr/local/opt/llvm` | Not verified on this task (no Intel Mac available); same keg-only layout, different prefix root — Homebrew's own convention. |
| Linux (apt.llvm.org, Debian/Ubuntu) | `/usr/lib/llvm-<N>/bin/clang++`, alternatives symlink `clang++-<N>` at `/usr/bin/clang++-<N>` | **Builds; suite passes** (tasks 6936, 7094: 4063 of 4063 ctest cases passed on `debian:trixie-slim`, arm64, 2026-09-30, at the M3 epic head). Measured against `debian:trixie-slim` with apt.llvm.org's `clang-23` / `libc++-23-dev` (prefix `/usr/lib/llvm-23`): the tree configures, compiles and links, and the full ctest run exits 0. `make linux-gate` (`docker/linux-gate.Dockerfile`, task 7094) is now the repeatable way to rerun it and it records the pass count; see [testing.md](testing.md#the-linux-gate). The Linux flag set is in § Linux flag set below; the test-only `sqlite3` CLI dependency is task 6944. |

## How the prefix is found (task 6755, decision 1123)

`CMakePresets.json` no longer carries absolute compiler paths. Its `base`
preset names a toolchain file, `cmake/llvm-toolchain.cmake`, which
DISCOVERS the prefix and derives the flag set below against it.

**The pin is unchanged.** Discovery changes how the toolchain is located,
not which toolchain is required: a candidate is accepted only if it has
`bin/clang`, `bin/clang++`, `include/c++/v1`, a `libc++.modules.json`
(the artifact that proves this libc++ was built with module support —
without it `import std` cannot work), and a clang major at or above the
pinned floor. Anything else is REFUSED with a message naming the fix. A
stock system clang fails on the modules manifest and is never silently
accepted.

Resolution order, mirroring the lint path's (`Makefile` § `LLVM_PREFIX`):

1. `-DPLANAR_LLVM_PREFIX=<path>` (or the same name in the environment)
   ALWAYS wins, unconditionally. Discovery is a fallback, never an
   override. An explicit prefix that fails validation is a hard error
   naming what was wrong with it — it never falls through to discovery.
2. `brew --prefix llvm`. Homebrew's LLVM is keg-only, so it is never on
   `PATH` and this is the only way to find it. This is what resolves on
   the macOS row of the table above.
3. apt.llvm.org's `/usr/lib/llvm-<major>` prefixes, newest first, then the
   prefix of a `PATH`-resolved `clang++` / `clang++-<major>`. This is what
   the Linux row is meant to resolve through.

The resolved prefix is written to the cache as `PLANAR_LLVM_PREFIX`, which
is also where `make cpp-lint` now reads it from — so the formatter and the
compiler still cannot drift apart.

**An ALREADY-CONFIGURED build directory is unaffected.** CMake reads a
toolchain file only when a build tree is first configured; re-running
`cmake --preset debug` over a `build/debug` created before this change
keeps its cached compiler paths and prints `CMake Warning (unused-cli):
CMAKE_TOOLCHAIN_FILE`. That is expected, and harmless on the host those
paths came from. Delete the build directory to pick up discovery.

**The same staleness bites when Homebrew moves the keg.** The cache
stores the tools by ABSOLUTE cellar path
(`/opt/homebrew/Cellar/llvm/<version>/bin/clang-scan-deps`,
`.../llvm-ar`, ...), not through the `/opt/homebrew/opt/llvm` symlink.
Homebrew installs every bottle revision into its own cellar directory and
removes the previous one, so a package-revision bump that changes no
compiler bits at all — `23.1.1` → `23.1.1_1` on 2026-09-21 — leaves an
idle build tree pointing at a directory that no longer exists. The
symptom is a build step failing with `code=127` and
`/bin/sh: /opt/homebrew/Cellar/llvm/<old>/bin/clang-scan-deps: No such
file or directory`, usually from the first third-party target
(`_deps/spdlog-build/...`) because that is what ninja reaches first. It
looks like a missing tool; it is a stale path. `rm -rf build/<preset>`
and re-run the configure or `make install`; discovery resolves the new
prefix. EVERY build tree configured before the bump is affected —
`build/debug` and `build/release` alike — and each one fails only when it
next has something to compile, so `make test` can keep passing on a tree
whose objects are all up to date while `make install` fails on the other
tree the same afternoon (2026-09-21: `release` failed first, `debug`
failed an hour later on `make surface-lint`, which needed to rebuild the
lint tool). Re-running configure over the stale tree is NOT enough: the
cached tool paths survive it (see the previous paragraph). Delete it.

**Linux builds, but is not yet a counted host.** Everything above was
verified on macOS ARM (see the probe transcript below). The Linux branch
has since been exercised against apt.llvm.org's `/usr/lib/llvm-23`
(task 6936): discovery accepts it, and the tree builds and runs its suite.
See the platform table's Linux row for what was and was not measured.

## Portable distribution builds

`PLANAR_PORTABLE` defaults to `OFF`. The `debug` and `release` presets keep
their shared C++ runtime link and toolchain rpath. The `dist` preset inherits
`release`, enables `PLANAR_PORTABLE` and `PLANAR_VERSION_META`, reads
`PLANAR_RELEASE_VERSION` from the environment, and sets the macOS deployment
target to `26.0` (the setting has no effect on Linux). A tagged cut can pass
its version explicitly:

```sh
cmake --preset dist -DPLANAR_RELEASE_VERSION=v0.1.0
cmake --build --preset dist
```

Portable linking uses `-nostdlib++` and the resolved toolchain library
directory's `libc++.a` and `libc++abi.a`, plus `libunwind.a` on Linux. The
archives follow target libraries on the link line, and the toolchain rpath
is omitted. Linux uses `--unwindlib=none` to prevent Clang from adding its
default shared `libgcc_s` unwinder alongside the explicit static archive;
compiler builtins and startup objects remain enabled. The native host system
selects the unwinder when CMake first reads the toolchain before initializing
the target system name; an explicitly configured cross target takes precedence.
The first-read regression probe runs on Linux with
`cmake -DPLANAR_LLVM_PREFIX=/usr/lib/llvm-23 -P scripts/toolchain-probes/probe_portable_linux.cmake`. On macOS each archive uses Apple's `-load_hidden` linker option:
system frameworks load Apple's libc++ transitively, and exposing the pinned
runtime's globals caused an invalid free in `locale::~locale` during CLI
startup. Hiding the archive symbols keeps the two runtimes' state separate.
Configure refuses a missing archive and names its path. Switching
`PLANAR_PORTABLE` back to `OFF` restores the shared runtime link. The
macOS distribution floor is macOS 26.0; Linux bundles target glibc 2.36.
On Linux portable builds, curl discovers static OpenSSL archives and disables
configure-host CA bundle detection. It uses `/etc/ssl/certs` on the runtime
host and enables OpenSSL's default trust-store fallback. The portable Linux
HTTP transport clears curl 8.7.1's literal `none` CA filename (CPM passes a
scoped normal variable, while curl removes only the cache entry). It also
loads `/etc/pki/tls/certs/ca-bundle.crt` when present: curl's OpenSSL fallback
does not run when a CA directory is configured. Certificate and hostname
verification remain enabled. Ordinary Linux builds
retain curl's defaults, and macOS continues to use SecureTransport.
Certificate verification failures report `CertificateVerificationFailed` in
external sync results and event detail; other transport failures retain
`TransportFailed`. Removing a trusted host CA must produce that certificate
diagnostic as well as reject the request.

Because OpenSSL is frozen into a portable Linux binary, TLS fixes reach operators
through a new Planar release. When the system OpenSSL's static pkg-config
dependencies include zstd (as on Trixie with OpenSSL 3.5), configure requires
`libzstd.a` and resolves that dependency to the archive as well. Binary
dependency and clean-host trust checks
remain required release gates. `scripts/test-portable-tls.py` exercises the
Debian CA directory, removed CA, and Red Hat bundle against prebuilt Linux
binaries and existing Docker images. Its required arguments select the binary
directory, evidence directory, toolchain image (Python and OpenSSL), and bare
runtime image; run it through the host queue. The removed-CA case requires
`outcome=error` and `detail=CertificateVerificationFailed`, with no remote title.
The sync result remains exit 0, as for other per-link adapter errors.

The `portable.binaries` ctest case is registered only with `PLANAR_PORTABLE`.
It logs each of the five product binaries, inspects macOS dependencies and load
commands with `otool`, or Linux dynamic entries and symbol versions with
`readelf`, and then runs each CLI's `--help` in a disposable HOME/database.
This startup check also detects static-runtime initialization failures that
load-command inspection alone cannot find. macOS permits system frameworks
and libSystem, requires `LC_BUILD_VERSION minos 26.0`, and refuses `LC_RPATH`.
Linux permits only glibc and its named companions, refuses both `RPATH` and
`RUNPATH`, and requires the highest GLIBC symbol version to be at most 2.36.
Both platforms reject toolchain-prefix references and shared C++/TLS runtimes.
The companion `portable.inspector` case exercises positive and rejected
inspection fixtures on either host. Run both with
`ctest --test-dir build/dist -L '^portable$' --output-on-failure`; verify the
matched count is two. `debug` and `release` register neither case. A build on
a newer Linux distribution may correctly fail the GLIBC floor; fixtures do
not substitute for the release's required Bookworm build and clean-host gate.

## Derived import-std / embed flag set (macOS, verified)

Configure-time flags, mirroring the pattern in `centurion/CMakeLists.txt`
and `centurion/CMakePresets.json`'s `base` preset (Homebrew LLVM is
keg-only, so nothing here is discoverable from a bare `clang++` on `PATH`).
Since task 6755 these are produced by `cmake/llvm-toolchain.cmake` against
the DISCOVERED prefix rather than written literally into the preset; the
values below are what that resolves to on this machine, and the libc++
library directory is read out of `libc++.modules.json`'s location rather
than assumed to be `lib/c++`. Three layouts are accepted: Homebrew's
`lib/c++/libc++.modules.json`, apt.llvm.org's `lib/libc++.modules.json`
directly under `lib/` (for example `/usr/lib/llvm-23/lib/libc++.modules.json`),
and `lib/<subdir>/libc++.modules.json`. Before task 6936 only the first and
last were globbed, so the apt.llvm.org layout was refused as "built without
module support" (the macOS values below are unchanged by that fix):

```
CMAKE_EXPERIMENTAL_CXX_IMPORT_STD = f35a9ac6-8463-4d38-8eec-5d6008153e7d   # CMake 4.4.x gate UUID
CMAKE_C_COMPILER                 = /opt/homebrew/opt/llvm/bin/clang
CMAKE_CXX_COMPILER               = /opt/homebrew/opt/llvm/bin/clang++
CMAKE_CXX_FLAGS                  = -stdlib=libc++ -nostdinc++ -isystem /opt/homebrew/opt/llvm/include/c++/v1
CMAKE_EXE_LINKER_FLAGS           = -stdlib=libc++ -L/opt/homebrew/opt/llvm/lib/c++ -Wl,-rpath,/opt/homebrew/opt/llvm/lib/c++
CMAKE_CXX_STDLIB_MODULES_JSON    = /opt/homebrew/opt/llvm/lib/c++/libc++.modules.json
```

### Linux flag set

`cmake/llvm-toolchain.cmake` derives the same flag set on Linux as on macOS;
it appends nothing per platform, which `make linux-gate` proves. Against
`/usr/lib/llvm-23` the resolved
values are:

```
CMAKE_CXX_FLAGS        = -stdlib=libc++ -nostdinc++ -isystem /usr/lib/llvm-23/include/c++/v1
CMAKE_EXE_LINKER_FLAGS = -stdlib=libc++ -L/usr/lib/llvm-23/lib -Wl,-rpath,/usr/lib/llvm-23/lib
CMAKE_CXX_STDLIB_MODULES_JSON = /usr/lib/llvm-23/lib/libc++.modules.json
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
clang 23.1.0 (re-verified; identical under the earlier 22.1.8 pin) emits
`-Wc23-extensions` ("#embed is a Clang extension") in
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
`Homebrew clang-format version 23.1.0` — matching the pinned compiler major
and patch exactly, as it must (same Homebrew keg). CMake tooling and any
pre-commit/format-check script must reference this path explicitly (e.g. via
`CMAKE_CXX_COMPILER`-relative derivation, the same pattern the
`CMAKE_CXX_STDLIB_MODULES_JSON` discovery in `tabula`'s top-level
`CMakeLists.txt` uses), not a bare `clang-format` invocation.

`make cpp-lint` implements exactly that derivation (plan 996, task 6054).
It resolves the prefix from `build/debug`'s `CMakeCache.txt`
(`PLANAR_LLVM_PREFIX`, else `CMAKE_CXX_COMPILER` up two directories), so the formatter is by
construction the same LLVM that built the tree; falls back to
`brew --prefix llvm` when no build directory exists; honours an explicit
`LLVM_PREFIX=` / `CLANG_FORMAT_BIN=` / `CLANG_TIDY_BIN=` override; and
**fails loudly** with the override to set rather than falling back to a
`PATH` binary. The literal `/opt/homebrew/opt/llvm/...` paths it used to
carry made the target Homebrew-ARM-macOS-only.

## Break-probes against the C++ tree

`scripts/break-probe.sh` runs one break-probe — mutate, rebuild, assert
the named ctest case fails, restore, rebuild, assert it passes — and
enforces the five checks that otherwise make a probe pass silently
meaningless:

| Trap | What it looks like | What the script does |
|---|---|---|
| Restoring a backup with `mv F.bak F` puts back an **older** mtime, so ninja skips the rebuild and the **mutant stays linked** | "all mutants killed", from a binary that was never rebuilt | restores by content and `touch`es, then rebuilds and re-runs the test to prove the restore took |
| The mutation's anchor matched zero occurrences | a green test, indistinguishable from a survivor | diffs each `--file` against its backup and refuses a no-op mutation |
| The mutant does not compile | looks like a kill | fails loudly; a compiler-rejected mutant is not a kill |
| `ctest -R` matched zero tests | proves nothing, either exit code | asserts the filter names at least one real ctest test (Catch2 **tags** are not ctest names) |
| The mutation changed the file but **not the behaviour** — it landed on a comment, whitespace, dead code, or an unreachable branch | a green test reported as `SURVIVOR`, indicting a test that was never actually challenged | hashes the behaviour-bearing sections (`__text`, `__cstring`, `__data`) of every linked binary before and after the mutant build, and reports **`INERT`** as a verdict distinct from `SURVIVOR` |

```
scripts/break-probe.sh \
  --file src/cmd/planar-watch/handlers/ps/command.cppm \
  --label "drop --json from the 'ps' node" \
  --mutate "sed -i '' 's|^  shared::add_json(\*ps);$||' src/cmd/planar-watch/handlers/ps/command.cppm" \
  --test 'planar-watch parity: every command declares what the oracle declares'
```

Exit 0 is `killed`, exit 1 is `SURVIVOR`, exit 3 is `INERT`, exit 2 is a
broken probe. A survivor is a finding about the test, not a footnote —
see [`agents/methodology.md`](../agents/methodology.md) § Break-probe
discipline.

`INERT` is deliberately not folded into `SURVIVOR`. The two look
identical from the test's point of view — the mutant builds, the named
test stays green — and mean opposite things: a survivor indicts the
**test**, an inert mutant indicts the **probe**. Reporting the second as
the first is how a test acquires evidence it never earned, so an `INERT`
result means "re-aim the mutation at a line that actually executes", not
"the test is weak".

Getting that discriminator right needed measurement rather than
reasoning; two plausible-looking versions were wrong (task 6336, on this
toolchain):

- Hashing the `.o` files does **not** work. A module interface unit's
  object embeds the BMI, which carries the source text, so editing a
  comment in a `.cppm` changes its `.o` — the check would have called
  the exact mutation it exists to catch "behavioural".
- Hashing the linked executable fails too, with or without
  `llvm-strip`: a fully stripped Mach-O still differed after a
  comment-only edit.
- Dumping only the loadable sections works: across a comment-only edit
  every byte of `__text`/`__cstring`/`__data` was identical, while a
  one-line behavioural edit moved thousands of them.

The check is conservative by construction — it claims `INERT` only on
byte-identical sections, so residual build nondeterminism can only cost
a missed `INERT`, never a false one — and it refuses to run at all if it
dumps no sections, so the discriminator cannot silently match nothing.

## Validation evidence

All commands below ran in the foreground on this machine (macOS, Apple
Silicon, Homebrew) on 2026-08-21 against the then-pinned LLVM 22.1.8, and
were re-run and reproduced identically on 2026-09-04 against the now-pinned
LLVM 23.1.0 (see "Why 23, and the 22 interim" above). Probe sources are committed at
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

## Linux release toolchain (task 7304)

The Docker `dist-toolchain` stage uses `debian:bookworm-slim`, independently
of the Trixie `toolchain` and `gate` stages. It installs the pinned LLVM major
23 from apt.llvm.org's `llvm-toolchain-bookworm-23` repository, including
`libc++-23-dev`, `libc++abi-23-dev` and `libunwind-23-dev`, and Bookworm's
`libssl-dev`. The stage checks the modules manifest and all three static
runtime archives before configuring a portable build. An unavailable repository
or package fails the image build; it never substitutes a newer Debian base.
`LLVM_APT_URL` is a build argument for probing an unavailable repository; its
default is `https://apt.llvm.org` and it affects only the release toolchain.

The M1 prerequisite was measured on 2026-10-06 on an Apple silicon host with
Docker Desktop 4.93.0, targeting `linux/amd64`. Bookworm supplied glibc
2.36-9+deb12u14 and OpenSSL 3.0.22-1~deb12u1; apt.llvm.org supplied LLVM
23.1.2 (`1:23.1.2~++20260920033443+85ac56026243-1~exp1~20260920033605.79`).
CMake 4.4.2 used the existing x86_64 SHA-256 pin. The cold portable product
build ran 1,997 steps with four jobs; image setup, configure, build and export
took 889 seconds. Both `portable`-labelled tests subsequently passed, inspecting
all five binaries for startup, glibc-only shared dependencies, no rpath and
no GLIBC requirement above 2.36. Full diagnostics and package/archive provenance
are retained in `build/m1-evidence/task7304/` on the dispatch host. This is
product-build evidence; bundle assembly and clean-container bundle health
validation depend on the later `make dist` implementation.

`make linux-dist` fixes the platform to `linux/amd64` on either host, uses a
separate Bookworm build cache, invokes `make dist` inside the image and exports
`/out/dist` contents into `dist/` (override `LINUX_DIST_OUT` for evidence).
`LINUX_DIST_JOBS` defaults to four. Apple silicon uses amd64 emulation.
The Trixie debug gate retains its platform, commands and cache.

The host wrapper resolves the full HEAD SHA and dirty state before Docker
copies the source snapshot without `.git`. A supplied `PLANAR_RELEASE_VERSION`
must name an existing tag at that exact clean HEAD; missing tags, mismatched
HEAD and dirty tagged cuts fail before Docker starts. The stage passes
`PLANAR_RELEASE_VERSION`, `PLANAR_SOURCE_SHA` and `PLANAR_SOURCE_DIRTY` through
to `make dist`. Its assembler must validate and embed that supplied identity in
both binaries and `release.json`; it must not infer identity from the git-free
container. Do not edit the source while the queued build waits or runs.

## Bundle assembly

`make dist` configures and builds the native `dist` preset, installs the five
binaries into an owned temporary staging tree, and assembles
`dist/planar-macos-arm64.tar.gz` or `dist/planar-linux-x86_64.tar.gz`.
`JOBS` defaults to four. Only these two platforms are supported. The bundle
contains the authored skill, agents, templates, workflows, migrations,
installer and install library; Codex agents are rendered during assembly.
The root bootstrap and uninstaller join the bundle when their milestones
supply them. Bundle assembly does not change the installer's lifecycle.

For a tagged cut, set `PLANAR_RELEASE_VERSION=vMAJOR.MINOR.PATCH`. The tag
must already exist at clean HEAD; pre-release labels, missing tags, wrong HEAD,
foreign untracked source and tracked edits refuse before building. An unset
version produces `dev`, which cannot be published. Only owned build and dist
outputs are ignored, so repeated cuts retain clean source identity. The native
assembler and `linux-dist` share the same identity check. Git-free Docker builds
consume the trusted host's full `PLANAR_SOURCE_SHA` and `PLANAR_SOURCE_DIRTY`;
CMake embeds them, and the assembler requires matching SHA, release, dirty state
and date from all four version-bearing staged binaries.

`release.json` has seven flat fields, one per line. Its database schema comes
from successful staged `init --skip-project --allow-no-repo` and `health --json`
in an owned scratch HOME, PLANAR_DB and PLANAR_CONFIG_PATH outside the
source worktree, with inherited Git-directory overrides removed for these probes. Health must report
current schema and numerically equal database and target versions. The target's
serialized type is preserved; the CLI catalog format version is never used.
The scratch arena is removed on success and refusal. Tar entries are sorted,
with uid/gid zero and owner/group `root`; compiled bytes and dates need not be
identical across cuts. `SHA256SUMS` uses two spaces before each bare asset name,
and `VERSION` contains the bare tag (or `dev`).

Assembly requires exactly the two `portable` tests and inspects all five
installed copies again. A sibling `<archive>.gates.json` records
`format_version: 1`, the archive name, SHA-256, the complete `release` identity,
and `gates.portable` with `result: "pass"`, `matched_count: 2` and
`staged_binaries: 5`. It binds portability evidence to that exact archive.
It contains no smoke or CA verdict: the later common publisher must require
successful toolchain-free smoke and applicable CA trust evidence for both
platforms, each bound to the same final checksum and release identity, and
must refuse missing evidence or `dev`. Assembly does not publish anything.
