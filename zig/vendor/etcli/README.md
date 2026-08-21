# etcli

`etcli` is a standalone Zig package providing a comptime-driven CLI parser.
It gives tools typed command trees, generated help, shell completion scripts,
man pages, and machine-readable command schemas, with no dependencies beyond
Zig `std`.

Requires Zig `0.16.x`.

## Install

Fetch the package into your project's `build.zig.zon` (pin to a released tag):

```sh
zig fetch --save "git+https://github.com/rdrsss/etcli#v0.2.0"
```

That records the dependency under the name `etcli`. Then import it under
whatever local name you prefer — the package exposes the same root under both
`cli` (the canonical short name used throughout these docs) and `etcli`:

```zig
const cli = @import("cli");
```

In the consuming project's `build.zig`, wire the dependency module into an
executable or library:

```zig
const etcli_dep = b.dependency("etcli", .{
    .target = target,
    .optimize = optimize,
});

exe.root_module.addImport("cli", etcli_dep.module("cli"));
```

## Example

```zig
const std = @import("std");
const cli = @import("cli");

const root = cli.Cmd{
    .name = "tool",
    .cmds = &.{
        .{
            .name = "hello",
            .desc = "Print a greeting",
            .flags = &.{
                .{
                    .long = "--name",
                    .short = 'n',
                    .kind = .string,
                    .default = .{ .string = "world" },
                },
            },
            .run = cli.handler(handleHello),
        },
    },
};

comptime cli.validate(root);

fn handleHello(args_ptr: *const anyopaque) anyerror!void {
    const args = cli.castArgs(root, &.{ "hello" }, args_ptr);
    std.debug.print("hello, {s}\n", .{args.name});
}
```

The snippet above is a fragment focused on the command tree. For a complete,
compilable program — including `pub fn main`, argv acquisition, and dispatch —
see [`examples/basic.zig`](examples/basic.zig).

For applications that want a standard top-level policy, `cli.run` owns parse
errors, help output, version/about output, handler-error formatting, and exit
codes while leaving the lower-level `parse` and `dispatch` APIs unchanged:

```zig
const code = try cli.run(root, .{
    .argv = argv,
    .stdout = stdout,
    .stderr = stderr,
    .version = "1.0.0",
    .about = "tool performs work",
});
```

Runner policy is deliberately narrow: `--help`, no-handler help, `--version`,
and `--about` write stdout and return `0`; parse errors write stderr and return
`2`; handler errors write stderr and return `1`. The runner also owns the
`Flag.env` fallback described below and warns on stderr when the invoked command
or a used flag is deprecated.

### Colorized help

`cli.run` can colorize generated help. Because help is built at comptime, the
colored and plain forms are distinct `.rodata` strings and the runner picks one
at runtime:

```zig
const code = try cli.run(root, .{
    .argv = argv,
    .stdout = &stdout_writer.interface,
    .stderr = &stderr_writer.interface,
    .color = .auto, // .auto | .always | .never (default .auto)
    .stdout_tty = std.Io.File.stdout().isTty(init.io) catch false,
});
```

`.auto` colorizes only when `stdout_tty` is true and `NO_COLOR` is unset
(resolved through `env_lookup` when provided). The writer abstraction hides the
file descriptor, so the caller reports TTY state via `stdout_tty`. Section
headers render bold and command/flag names render in cyan; column alignment is
computed from the uncolored label, so layout is identical with or without color.

## Aliases and Visibility

Commands and flags can declare aliases, hidden status, and deprecation metadata.
Aliases parse to canonical command paths and canonical flag fields, so generated
argument names remain stable during migrations:

```zig
.{ .name = "run", .aliases = &.{"go"} }
.{ .long = "--name", .aliases = &.{"--title"}, .kind = .string }
```

Hidden commands and flags remain parseable but are omitted from help,
completion, man pages, and schema output unless the generator option includes
hidden items. Deprecated items remain parseable and render deprecation metadata
in generated docs/schema; `cli.run` also warns when a deprecated command is
invoked. Flag-level deprecation warnings remain a later runner policy.

## Flag Values

String and integer flags accept both common value forms:

```sh
--name value
--name=value
-nvalue
```

Boolean flags accept presence, explicit values, negation, and unambiguous short
bundles:

```sh
--verbose
--verbose=true
--verbose=false
--no-color
-vf
```

Attached short values are accepted for non-bool short flags, for example
`-nname` and `-c3`. Short bundles are only accepted when every bundled short
flag is boolean; ambiguous forms fail as unknown flags. Scalar flags still reject
duplicates.

Flags and positionals can attach a custom `validator` that receives the raw
argument string after kind routing; return `null` to accept it, or a message to
reject it as `InvalidValue`. List-valued flags are supported for `.string`,
`.path`, and `.choice` values by setting `.list = true`; they may repeat and
generate `[]const []const u8` fields with an empty default. Optional
positionals can declare defaults, which fill the generated field when the slot
is omitted.

Count flags accumulate their occurrences instead of erroring on repeats. Set
`.count = true` on a `.bool` flag and its generated field becomes a `u32`
(default `0`) that increments once per occurrence — `-vvv` and
`--verbose --verbose --verbose` both yield `3`:

```zig
.{ .long = "--verbose", .short = 'v', .kind = .bool, .count = true, .desc = "Increase verbosity" }
```

Count flags take no value and have no `--no-` negation; they are mutually
exclusive with `list`, `default`, `required`, and `value_name`.

Commands can also declare flag groups as command metadata. Groups reference the
canonical long names of flags visible at that command path, including inherited
flags:

```zig
const root = cli.Cmd{
    .name = "tool",
    .flags = &.{
        .{ .long = "--verbose", .kind = .bool },
    },
    .cmds = &.{
        .{
            .name = "run",
            .flags = &.{
                .{ .long = "--json", .kind = .bool },
                .{ .long = "--yaml", .kind = .bool },
                .{ .long = "--text", .kind = .bool },
            },
            .flag_groups = &.{
                .{
                    .name = "output-format",
                    .mode = .required_exactly_one,
                    .flags = &.{ "--json", "--yaml", "--text" },
                    .desc = "Choose one output format.",
                },
                .{
                    .name = "run-input",
                    .mode = .required_one,
                    .flags = &.{ "--verbose", "--json" },
                },
            },
        },
    },
};
```

The initial group modes are `.mutually_exclusive`, `.required_one`, and
`.required_exactly_one`. `cli.validate` rejects duplicate group names on the
same command, empty or duplicate member lists, unknown or non-canonical member
references, partial hidden/deprecated visibility groups, and exclusive groups
that cannot be satisfied because multiple members are individually required.
The parser enforces group modes on the matched command path, and generated help,
man pages, and schema JSON render the group metadata for visible command pages.

Beyond `.bool`, `.string`, and `.int`, flags and positionals support `.float`
(`f64`), `.duration` (human strings like `10m`/`500ms`/`1h` parsed to
nanoseconds), and `.path` (a string that auto-completes files). Flags also
support `.choice` (see below).

## Choice Flags

A `.choice` flag constrains its value to a declared set. Membership is enforced
at parse time (with a nearest-match suggestion on a miss) and the set is checked
at compile time — non-empty, unique, shell-safe, and any default must be a
member:

```zig
.{ .long = "--format", .short = 'f', .kind = .choice,
   .choices = &.{ "json", "text", "yaml" }, .default = .{ .choice = "text" } }
```

The generated args field is `[]const u8`, guaranteed to hold one of the
choices. The set is declared once and drives everything: it auto-populates shell
completion and renders as `(json|text|yaml)` in help, as the value placeholder
in man pages, and as a `"choices"` array in the command schema.

## Environment Fallback

The low-level `parse`/`dispatch` APIs never read the environment. The `cli.run`
runner, however, applies `Flag.env` as a fallback when you pass an `env_lookup`
function:

```zig
const code = try cli.run(root, .{
    .argv = argv,
    .stdout = stdout,
    .stderr = stderr,
    .env_lookup = struct {
        fn lookup(name: []const u8) ?[]const u8 {
            return std.posix.getenv(name);
        }
    }.lookup,
});
```

For each visible flag on the resolved command path with `.env` set and absent
from argv, the runner fills the value from the environment before parsing, so
precedence is `argv` > env > declared default > required-flag error. Inherited
root and parent flags, leaf-local flags, bool flags, list flags, and scalar
value flags all flow through the existing parser coercion and validation.

## Completion Scripts

Completion scripts are generated at comptime for bash, zsh, and fish. Command
and flag descriptions are escaped for zsh and fish completion output so common
description text containing quotes or colons remains valid shell syntax. Bash
completion uses `compgen`, which does not expose a portable description column;
generated bash scripts therefore prioritize correct candidates and value
completion.
Flags and positionals can also declare static value completions:

```zig
.{ .long = "--mode", .completion = cli.Completion.valueChoices(&.{ "json", "text" }) }
.{ .long = "--input", .completion = cli.Completion.files }
.{ .name = "target", .completion = cli.Completion.valueChoices(&.{ "alpha", "beta" }) }
```

Static value choices are embedded directly into generated bash, zsh, and fish
scripts. File and directory completions use each shell's native file completion
behavior where possible.

Value completion covers separated values, long equals forms such as
`--mode=j`, and short value forms such as `-m j` or `-mj`. Flag values consumed
for completion do not become command-path tokens.

For values only known at runtime, declare a dynamic completion callback:

```zig
.{ .long = "--host", .completion = cli.Completion.dynamic(completeHosts) }

fn completeHosts(prefix: []const u8) []const []const u8 {
    // ... compute candidates for `prefix` ...
}
```

The generated scripts invoke the program's `__complete` builtin for dynamic
flags. `cli.run` wires that builtin automatically; to wire it yourself (e.g.
under `dispatch`), route `<prog> __complete <flag> <prefix>` to `cli.complete`.

## Man Pages

Man pages are generated at comptime from the same command tree:

```zig
const page = comptime cli.man.page(root, &.{ "hello" }, .{
    .title = "TOOL-HELLO",
    .source = "tool 1.0",
    .manual = "User Commands",
});
```

The generator infers command paths, subcommands, inherited flags, local flags,
flag kinds, defaults, required markers, positionals, `desc`, and `long_desc`
from `Cmd`, `Flag`, and `Positional` declarations. Use `Flag.value_name` when a
string or integer flag should render a domain-specific placeholder such as
`PATH` or `COUNT`; otherwise the generator falls back to `VALUE` for strings
and `N` for integers.

Manual-only content belongs in `Cmd.doc`. Examples, exit statuses, notes, and
see-also references enrich generated man pages but do not affect parsing,
dispatch, or generated argument types.

`Flag.env` renders in the man page ENVIRONMENT section as a `cli.run` fallback
source. The parser still does not read environment variables, and generated
text says so explicitly.

## Packaging Artifacts

Downstream projects can write generated package artifacts from an opt-in build
or packaging step. A common shape is a tiny generator executable that
imports the application's command tree, calls `cli.artifacts`, and hands each
artifact to project-owned staging code:

```zig
const gen_docs = b.addExecutable(.{
    .name = "gen-docs",
    .root_source_file = b.path("tools/gen_docs.zig"),
    .target = target,
    .optimize = optimize,
});
gen_docs.root_module.addImport("cli", etcli_dep.module("cli"));

const run_gen_docs = b.addRunArtifact(gen_docs);
run_gen_docs.addArg("zig-out/package-root");

const docs_step = b.step("dist-docs", "Stage package artifacts");
docs_step.dependOn(&run_gen_docs.step);
```

The generator executable can then stage deterministic man pages, shell
completions, and schema artifacts. The artifact values are plain data plus
advisory metadata; the downstream helper decides whether to create directories,
write files, gzip man pages, or translate the hint for a package manager:

```zig
const man_pages = comptime cli.artifacts.allManPages(root, .{});
for (man_pages) |artifact| {
    try stageArtifact(allocator, staging_dir, artifact);
}

try stageArtifact(
    allocator,
    staging_dir,
    comptime cli.artifacts.completionScript(root, .bash),
);
try stageArtifact(
    allocator,
    staging_dir,
    comptime cli.artifacts.completionScript(root, .zsh),
);
try stageArtifact(
    allocator,
    staging_dir,
    comptime cli.artifacts.completionScript(root, .fish),
);
try stageArtifact(
    allocator,
    staging_dir,
    comptime cli.artifacts.schemaJson(root, .{}),
);
```

One possible staging helper can consume all public artifact fields while keeping
install policy outside `etcli`:

```zig
fn stageArtifact(
    allocator: std.mem.Allocator,
    staging_dir: std.fs.Dir,
    artifact: cli.artifacts.Artifact,
) !void {
    const destination = artifact.destination_hint;
    if (destination.len == 0) return error.MissingDestinationHint;

    const package_section = switch (artifact.category) {
        .man_page => "manuals",
        .bash_completion, .zsh_completion, .fish_completion => "completions",
        .schema_json => "schemas",
        .unknown => "misc",
    };

    try recordPackageEntry(.{
        .section = package_section,
        .destination = destination,
        .name = artifact.name,
    });

    try staging_dir.makePath(destination);
    const sub_path = try std.fs.path.join(
        allocator,
        &.{ destination, artifact.name },
    );
    defer allocator.free(sub_path);

    try staging_dir.writeFile(.{ .sub_path = sub_path, .data = artifact.data });
}
```

Artifact naming is deterministic: man pages use `tool.1` and
`tool-subcommand.1`, completions use `tool.bash`, `_tool`, and `tool.fish`, and
schema output uses `tool.schema.json`. `etcli` intentionally returns plain
text and never installs, compresses, or writes artifacts on its own. The
`destination_hint` values are conventional defaults such as `share/man/man1`,
bash-completion, zsh functions, fish vendor completions, and schema collection
directories; package recipes may map them to distribution-specific locations.

## Command Schema

LLMs and tool routers should consume structured schema output instead of man
pages. The schema is compact JSON generated at comptime from the same command
tree, so agents do not need to infer flags, positionals, requiredness, defaults,
or examples from prose:

```zig
const schema = comptime cli.schema.json(root, .{
    .include_inherited_flags = true,
    .include_docs = true,
});
```

The v1 schema uses a flat command list. Each command entry includes its path,
full command string, subcommands, flags, positionals, docs, and explicit
`cli-run-fallback` env behavior where `Flag.env` is present.

Schema compatibility is explicit: default schema output remains
`"schemaVersion": 1`, `"layout": "flat"`, and does not emit `commandTree`.
Consumers that need hierarchy can opt in with `.include_command_tree = true`.
That option adds a top-level `commandTree` whose nodes reuse the flat command
metadata and add `children`, while the flat `commands` list remains present.
Because tree output is additive and explicitly requested, it also keeps
`"schemaVersion": 1`.

Docs metadata also carries structured project fields for richer generated
manuals and schemas: `files`, `bugs`, `authors`, `homepage`, `license`,
`copyright`, `version`, and `source_url`. These fields are parser-neutral.
They only affect generated documentation and machine-readable catalogs.

## Validation

Call `comptime cli.validate(root);` near each command tree declaration. The
validator rejects duplicate subcommands, duplicate inherited flags, invalid
command/flag/positional syntax, generated args field-name collisions,
mismatched default kinds, invalid flag-group declarations, empty manual metadata
entries, and required flags that also define defaults. Manual metadata
validation covers examples, exit codes, notes, see-also entries, files, bugs,
and authors so generated docs cannot carry blank table rows.

Negative validation behavior is covered by compile-fail fixtures under
`integration_tests/compile_fail/`. The default `zig build test` step runs those
fixtures through `scripts/compile_fail.sh` and asserts the expected compile-time
diagnostics, including invalid handler signatures. Typed-args handler signatures
are intentionally rejected through `cli.handler`; handlers should accept
`*const anyopaque` and recover typed args with `cli.castArgs`.

## Tests

Run the extracted package's tests with:

```sh
zig build test
```

That command runs source-local unit tests, downstream-style import tests for both
`cli` and `etcli`, parser contract tests, dispatch tests, completion/help
tests, man-page generation tests, schema generation tests, and compile-fail
validation fixtures. Snapshot contract tests pin representative help, man,
completion, and schema output. Artifact contract tests inspect generated names,
data, categories, and destination hints, but normal CI/test runs do not create
install trees or leave staged man-page, completion, or schema files behind.
Artifact metadata remains advisory for downstream packaging code.

If `mandoc` is installed locally, the test step also runs strict
`mandoc -Tlint` over committed man-page snapshots; any warning or error fails
the test step. Otherwise that lint gate prints a skip message and succeeds.
Completion snapshots are linted with `bash -n`, `zsh -n`, and `fish -n` when
those shells are installed; missing optional shells print skip messages, but
missing bash, zsh, or fish snapshot coverage fails the gate. Run that check
directly with `zig build completion-lint` or `sh scripts/completion_lint.sh`.

See `examples/basic.zig` for a complete command tree and app-runner setup. CI
matrix guidance lives in `docs/ci.md`; release and API-versioning policy lives
in `docs/release.md`.

## Contributing

Bug reports and pull requests are welcome. See [CONTRIBUTING.md](CONTRIBUTING.md)
for the build/test loop and conventions, and [SECURITY.md](SECURITY.md) for
reporting security-sensitive issues.

## License

`etcli` is released under the [MIT License](LICENSE). Copyright (c) 2026
Manuel A. Rodriguez.
