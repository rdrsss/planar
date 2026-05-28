# etc-cli

`etc-cli` is a standalone Zig package for the comptime-driven CLI parser
extracted from Planar. It is intended to be shared by multiple tools that want
typed command trees, generated help, shell completion scripts, and a small
runtime parser with no dependencies beyond Zig `std`.

## Use

Add this package as a dependency from another Zig project, then import it under
the local name you want. The package exposes the same root under both `cli`
and `etc_cli`; `cli` matches Planar's existing module name.

```zig
const cli = @import("cli");
```

In the consuming project's `build.zig`, wire the dependency module into an
executable or library:

```zig
const etc_cli_dep = b.dependency("etc_cli", .{
    .target = target,
    .optimize = optimize,
});

exe.root_module.addImport("cli", etc_cli_dep.module("cli"));
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
`2`; handler errors write stderr and return `1`. Environment fallback and
deprecation warnings are not runner-owned today.

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
in generated docs/schema; runner warnings are reserved for a later policy.

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
duplicates. Enum/choice kinds, floats, path/duration kinds, custom validators,
flag groups, list-valued flags, and positional defaults are deferred API work;
use strings plus application validation for those cases today.

## Environment Metadata

`Flag.env` is currently reserved metadata. The parser does not read environment
variables, and an `env` setting does not satisfy a required flag. Consumers
should pass environment-derived defaults explicitly until env fallback behavior
is added as a deliberate feature. Precedence is therefore `argv`, then declared
defaults, then required-flag errors; `env` is not in the parse-time precedence
chain.

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
behavior where possible. Runtime/dynamic completion callbacks are explicitly
deferred; use static metadata or application-owned completion commands for now.

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

`Flag.env` renders in the man page ENVIRONMENT section as metadata only. The
parser still does not read environment variables, and generated text says so
explicitly.

Downstream projects can write generated pages from an opt-in build step. A
common shape is a tiny generator executable that imports the application's
command tree, calls `cli.artifacts`, and writes files under caller-provided
output directories:

```zig
const gen_man = b.addExecutable(.{
    .name = "gen-man",
    .root_source_file = b.path("tools/gen_man.zig"),
    .target = target,
    .optimize = optimize,
});
gen_man.root_module.addImport("cli", etc_cli_dep.module("cli"));

const run_gen_man = b.addRunArtifact(gen_man);
run_gen_man.addArg("zig-out/share/man/man1");

const man_step = b.step("man", "Generate man pages");
man_step.dependOn(&run_gen_man.step);
```

The generator executable can then write deterministic artifacts:

```zig
const man_pages = comptime cli.artifacts.allManPages(root, .{});
for (man_pages) |artifact| {
    try man_dir.writeFile(.{ .sub_path = artifact.name, .data = artifact.data });
}

const bash = comptime cli.artifacts.completionScript(root, .bash);
try completion_dir.writeFile(.{ .sub_path = bash.name, .data = bash.data });

const schema = comptime cli.artifacts.schemaJson(root, .{});
try schema_dir.writeFile(.{ .sub_path = schema.name, .data = schema.data });
```

Artifact naming is deterministic: man pages use `tool.1` and
`tool-subcommand.1`, completions use `tool.bash`, `_tool`, and `tool.fish`, and
schema output uses `tool.schema.json`. `etc-cli` intentionally returns plain
text and does not install, compress, or write artifacts during normal tests.
Packaging code should decide output directories, gzip policy, and installation
locations such as `share/man/man1`, bash-completion, zsh functions, fish vendor
completions, or schema collection directories.

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
metadata-only env behavior where `Flag.env` is present.

Docs metadata also carries structured project fields for richer generated
manuals and schemas: `files`, `bugs`, `authors`, `homepage`, `license`,
`copyright`, `version`, and `source_url`. These fields are parser-neutral.
They only affect generated documentation and machine-readable catalogs.

## Validation

Call `comptime cli.validate(root);` near each command tree declaration. The
validator rejects duplicate subcommands, duplicate inherited flags, invalid
command/flag/positional syntax, generated args field-name collisions,
mismatched default kinds, empty manual metadata entries, and required flags that
also define defaults. Manual metadata validation covers examples, exit codes,
notes, see-also entries, files, bugs, and authors so generated docs cannot carry
blank table rows.

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
`cli` and `etc_cli`, parser contract tests, dispatch tests, completion/help
tests, man-page generation tests, schema generation tests, and compile-fail
validation fixtures. Snapshot contract tests pin representative help, man,
completion, and schema output. If `mandoc` is installed locally, the test step
also runs `mandoc -Tlint` over committed man-page snapshots; otherwise that lint
gate prints a skip message and succeeds. Completion snapshots are linted with
`bash -n`, `zsh -n`, and `fish -n` when those shells are installed.

See `examples/basic.zig` for a complete command tree and app-runner setup. CI
matrix guidance lives in `docs/ci.md`; release and API-versioning policy lives
in `docs/release.md`.
