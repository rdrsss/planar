/// @file surface.cpp
/// @brief Implementation of `planar.cmd.planar.surface` — GENERATED, do not hand-edit.
///
/// Regenerate with `scripts/gen-cli-surface.py` (see that script and this
/// module's interface header for the provenance argument).

module planar.cmd.planar.surface;

import std;
import planar.cliapp.surface;

namespace planar::cmd {

using cliapp::flag_spec;
using cliapp::node_spec;
using cliapp::positional_spec;

namespace {

constexpr std::string_view k_path_0[]   = {"init"};
constexpr std::string_view k_path_1[]   = {"scope"};
constexpr std::string_view k_path_2[]   = {"assoc"};
constexpr std::string_view k_path_3[]   = {"plan"};
constexpr std::string_view k_path_4[]   = {"task"};
constexpr std::string_view k_path_5[]   = {"question"};
constexpr std::string_view k_path_6[]   = {"scenario"};
constexpr std::string_view k_path_7[]   = {"decision"};
constexpr std::string_view k_path_8[]   = {"artifact"};
constexpr std::string_view k_path_9[]   = {"annotate"};
constexpr std::string_view k_path_10[]  = {"promote"};
constexpr std::string_view k_path_11[]  = {"demote"};
constexpr std::string_view k_path_12[]  = {"workbench"};
constexpr std::string_view k_path_13[]  = {"workspace"};
constexpr std::string_view k_path_14[]  = {"ext"};
constexpr std::string_view k_path_15[]  = {"link"};
constexpr std::string_view k_path_16[]  = {"unlink"};
constexpr std::string_view k_path_17[]  = {"links"};
constexpr std::string_view k_path_18[]  = {"sync"};
constexpr std::string_view k_path_19[]  = {"resume"};
constexpr std::string_view k_path_20[]  = {"handoff"};
constexpr std::string_view k_path_21[]  = {"capture"};
constexpr std::string_view k_path_22[]  = {"audit"};
constexpr std::string_view k_path_23[]  = {"health"};
constexpr std::string_view k_path_24[]  = {"models"};
constexpr std::string_view k_path_25[]  = {"dashboard"};
constexpr std::string_view k_path_26[]  = {"spec"};
constexpr std::string_view k_path_27[]  = {"test-spec"};
constexpr std::string_view k_path_28[]  = {"config"};
constexpr std::string_view k_path_29[]  = {"templates"};
constexpr std::string_view k_path_30[]  = {"tree"};
constexpr std::string_view k_path_31[]  = {"search"};
constexpr std::string_view k_path_32[]  = {"local"};
constexpr std::string_view k_path_33[]  = {"skills"};
constexpr std::string_view k_path_34[]  = {"import"};
constexpr std::string_view k_path_35[]  = {"synthesize"};
constexpr std::string_view k_path_36[]  = {"version"};
constexpr std::string_view k_path_37[]  = {"completion"};
constexpr std::string_view k_path_38[]  = {"schema"};
constexpr std::string_view k_path_39[]  = {"report"};
constexpr std::string_view k_path_40[]  = {"bench"};
constexpr std::string_view k_path_41[]  = {"closure"};
constexpr std::string_view k_path_42[]  = {"run"};
constexpr std::string_view k_path_43[]  = {"groups"};
constexpr std::string_view k_path_44[]  = {"explore"};
constexpr std::string_view k_path_45[]  = {"workflow"};
constexpr std::string_view k_path_46[]  = {"feedback"};
constexpr std::string_view k_path_47[]  = {"scope", "show"};
constexpr std::string_view k_path_48[]  = {"scope", "suggest"};
constexpr std::string_view k_path_49[]  = {"scope", "use"};
constexpr std::string_view k_path_50[]  = {"scope", "pop"};
constexpr std::string_view k_path_51[]  = {"scope", "clear"};
constexpr std::string_view k_path_52[]  = {"assoc", "list"};
constexpr std::string_view k_path_53[]  = {"assoc", "create"};
constexpr std::string_view k_path_54[]  = {"assoc", "add"};
constexpr std::string_view k_path_55[]  = {"assoc", "remove"};
constexpr std::string_view k_path_56[]  = {"assoc", "members"};
constexpr std::string_view k_path_57[]  = {"assoc", "detect"};
constexpr std::string_view k_path_58[]  = {"plan", "create"};
constexpr std::string_view k_path_59[]  = {"plan", "show"};
constexpr std::string_view k_path_60[]  = {"plan", "list"};
constexpr std::string_view k_path_61[]  = {"plan", "update"};
constexpr std::string_view k_path_62[]  = {"plan", "edit"};
constexpr std::string_view k_path_63[]  = {"plan", "view"};
constexpr std::string_view k_path_64[]  = {"plan", "diff"};
constexpr std::string_view k_path_65[]  = {"plan", "review"};
constexpr std::string_view k_path_66[]  = {"plan", "link"};
constexpr std::string_view k_path_67[]  = {"plan", "next"};
constexpr std::string_view k_path_68[]  = {"plan", "recommend-strategy"};
constexpr std::string_view k_path_69[]  = {"plan", "divergence"};
constexpr std::string_view k_path_70[]  = {"plan", "recompute-status"};
constexpr std::string_view k_path_71[]  = {"plan", "closeout"};
constexpr std::string_view k_path_72[]  = {"plan", "step"};
constexpr std::string_view k_path_73[]  = {"plan", "descendants"};
constexpr std::string_view k_path_74[]  = {"task", "add"};
constexpr std::string_view k_path_75[]  = {"task", "show"};
constexpr std::string_view k_path_76[]  = {"task", "packet"};
constexpr std::string_view k_path_77[]  = {"task", "list"};
constexpr std::string_view k_path_78[]  = {"task", "update"};
constexpr std::string_view k_path_79[]  = {"task", "edit"};
constexpr std::string_view k_path_80[]  = {"task", "view"};
constexpr std::string_view k_path_81[]  = {"task", "diff"};
constexpr std::string_view k_path_82[]  = {"task", "review"};
constexpr std::string_view k_path_83[]  = {"task", "done"};
constexpr std::string_view k_path_84[]  = {"task", "cancel"};
constexpr std::string_view k_path_85[]  = {"task", "block"};
constexpr std::string_view k_path_86[]  = {"task", "link"};
constexpr std::string_view k_path_87[]  = {"task", "reopen"};
constexpr std::string_view k_path_88[]  = {"task", "touches"};
constexpr std::string_view k_path_89[]  = {"question", "add"};
constexpr std::string_view k_path_90[]  = {"question", "edit"};
constexpr std::string_view k_path_91[]  = {"question", "view"};
constexpr std::string_view k_path_92[]  = {"question", "diff"};
constexpr std::string_view k_path_93[]  = {"question", "review"};
constexpr std::string_view k_path_94[]  = {"question", "answer"};
constexpr std::string_view k_path_95[]  = {"question", "wontfix"};
constexpr std::string_view k_path_96[]  = {"question", "list"};
constexpr std::string_view k_path_97[]  = {"question", "show"};
constexpr std::string_view k_path_98[]  = {"question", "link"};
constexpr std::string_view k_path_99[]  = {"scenario", "add"};
constexpr std::string_view k_path_100[] = {"scenario", "edit"};
constexpr std::string_view k_path_101[] = {"scenario", "view"};
constexpr std::string_view k_path_102[] = {"scenario", "diff"};
constexpr std::string_view k_path_103[] = {"scenario", "review"};
constexpr std::string_view k_path_104[] = {"scenario", "verify"};
constexpr std::string_view k_path_105[] = {"scenario", "retire"};
constexpr std::string_view k_path_106[] = {"scenario", "list"};
constexpr std::string_view k_path_107[] = {"scenario", "show"};
constexpr std::string_view k_path_108[] = {"scenario", "link"};
constexpr std::string_view k_path_109[] = {"decision", "add"};
constexpr std::string_view k_path_110[] = {"decision", "show"};
constexpr std::string_view k_path_111[] = {"decision", "list"};
constexpr std::string_view k_path_112[] = {"decision", "accept"};
constexpr std::string_view k_path_113[] = {"decision", "supersede"};
constexpr std::string_view k_path_114[] = {"decision", "withdraw"};
constexpr std::string_view k_path_115[] = {"decision", "edit"};
constexpr std::string_view k_path_116[] = {"decision", "view"};
constexpr std::string_view k_path_117[] = {"decision", "diff"};
constexpr std::string_view k_path_118[] = {"decision", "review"};
constexpr std::string_view k_path_119[] = {"decision", "link"};
constexpr std::string_view k_path_120[] = {"artifact", "add"};
constexpr std::string_view k_path_121[] = {"artifact", "show"};
constexpr std::string_view k_path_122[] = {"artifact", "list"};
constexpr std::string_view k_path_123[] = {"artifact", "update"};
constexpr std::string_view k_path_124[] = {"artifact", "edit"};
constexpr std::string_view k_path_125[] = {"artifact", "view"};
constexpr std::string_view k_path_126[] = {"artifact", "diff"};
constexpr std::string_view k_path_127[] = {"artifact", "review"};
constexpr std::string_view k_path_128[] = {"artifact", "link"};
constexpr std::string_view k_path_129[] = {"annotate", "add"};
constexpr std::string_view k_path_130[] = {"annotate", "show"};
constexpr std::string_view k_path_131[] = {"annotate", "list"};
constexpr std::string_view k_path_132[] = {"annotate", "update"};
constexpr std::string_view k_path_133[] = {"annotate", "remove"};
constexpr std::string_view k_path_134[] = {"annotate", "tag"};
constexpr std::string_view k_path_135[] = {"annotate", "resolve"};
constexpr std::string_view k_path_136[] = {"annotate", "dismiss"};
constexpr std::string_view k_path_137[] = {"annotate", "archive"};
constexpr std::string_view k_path_138[] = {"annotate", "bulk-resolve"};
constexpr std::string_view k_path_139[] = {"annotate", "bulk-dismiss"};
constexpr std::string_view k_path_140[] = {"annotate", "bulk-archive"};
constexpr std::string_view k_path_141[] = {"annotate", "verify"};
constexpr std::string_view k_path_142[] = {"annotate", "sweep"};
constexpr std::string_view k_path_143[] = {"workbench", "lint"};
constexpr std::string_view k_path_144[] = {"workbench", "pull"};
constexpr std::string_view k_path_145[] = {"workbench", "push"};
constexpr std::string_view k_path_146[] = {"workbench", "status"};
constexpr std::string_view k_path_147[] = {"workbench", "resolve"};
constexpr std::string_view k_path_148[] = {"workbench", "sync"};
constexpr std::string_view k_path_149[] = {"workbench", "archive"};
constexpr std::string_view k_path_150[] = {"workbench", "restore"};
constexpr std::string_view k_path_151[] = {"workbench", "gc"};
constexpr std::string_view k_path_152[] = {"workbench", "list"};
constexpr std::string_view k_path_153[] = {"workbench", "publish"};
constexpr std::string_view k_path_154[] = {"workbench", "extract-questions"};
constexpr std::string_view k_path_155[] = {"workbench", "edit"};
constexpr std::string_view k_path_156[] = {"workspace", "init"};
constexpr std::string_view k_path_157[] = {"workspace", "doctor"};
constexpr std::string_view k_path_158[] = {"workspace", "routing"};
constexpr std::string_view k_path_159[] = {"workspace", "regenerate"};
constexpr std::string_view k_path_160[] = {"ext", "register"};
constexpr std::string_view k_path_161[] = {"ext", "list"};
constexpr std::string_view k_path_162[] = {"ext", "test"};
constexpr std::string_view k_path_163[] = {"ext", "create"};
constexpr std::string_view k_path_164[] = {"ext", "propagate-one"};
constexpr std::string_view k_path_165[] = {"ext", "propagate"};
constexpr std::string_view k_path_166[] = {"links", "add"};
constexpr std::string_view k_path_167[] = {"links", "list"};
constexpr std::string_view k_path_168[] = {"links", "remove"};
constexpr std::string_view k_path_169[] = {"links", "trail"};
constexpr std::string_view k_path_170[] = {"sync", "pull"};
constexpr std::string_view k_path_171[] = {"sync", "push"};
constexpr std::string_view k_path_172[] = {"sync", "status"};
constexpr std::string_view k_path_173[] = {"sync", "resolve"};
constexpr std::string_view k_path_174[] = {"resume", "validate"};
constexpr std::string_view k_path_175[] = {"handoff", "create"};
constexpr std::string_view k_path_176[] = {"handoff", "validate"};
constexpr std::string_view k_path_177[] = {"handoff", "consume"};
constexpr std::string_view k_path_178[] = {"handoff", "abandon"};
constexpr std::string_view k_path_179[] = {"handoff", "list"};
constexpr std::string_view k_path_180[] = {"handoff", "show"};
constexpr std::string_view k_path_181[] = {"capture", "session"};
constexpr std::string_view k_path_182[] = {"capture", "commits"};
constexpr std::string_view k_path_183[] = {"capture", "end"};
constexpr std::string_view k_path_184[] = {"capture", "note"};
constexpr std::string_view k_path_185[] = {"capture", "command"};
constexpr std::string_view k_path_186[] = {"capture", "file"};
constexpr std::string_view k_path_187[] = {"capture", "snapshot"};
constexpr std::string_view k_path_188[] = {"audit", "trail"};
constexpr std::string_view k_path_189[] = {"audit", "commits"};
constexpr std::string_view k_path_190[] = {"audit", "session"};
constexpr std::string_view k_path_191[] = {"audit", "publish-decision"};
constexpr std::string_view k_path_192[] = {"audit", "handoff-readiness"};
constexpr std::string_view k_path_193[] = {"health", "hygiene"};
constexpr std::string_view k_path_194[] = {"models", "evals"};
constexpr std::string_view k_path_195[] = {"models", "resolve"};
constexpr std::string_view k_path_196[] = {"models", "experiments"};
constexpr std::string_view k_path_197[] = {"models", "outcomes"};
constexpr std::string_view k_path_198[] = {"models", "registry"};
constexpr std::string_view k_path_199[] = {"spec", "ingest"};
constexpr std::string_view k_path_200[] = {"test-spec", "status"};
constexpr std::string_view k_path_201[] = {"config", "show"};
constexpr std::string_view k_path_202[] = {"config", "edit"};
constexpr std::string_view k_path_203[] = {"config", "validate"};
constexpr std::string_view k_path_204[] = {"config", "init"};
constexpr std::string_view k_path_205[] = {"config", "path"};
constexpr std::string_view k_path_206[] = {"templates", "list"};
constexpr std::string_view k_path_207[] = {"templates", "show"};
constexpr std::string_view k_path_208[] = {"templates", "render"};
constexpr std::string_view k_path_209[] = {"templates", "validate"};
constexpr std::string_view k_path_210[] = {"templates", "init"};
constexpr std::string_view k_path_211[] = {"templates", "path"};
constexpr std::string_view k_path_212[] = {"local", "list"};
constexpr std::string_view k_path_213[] = {"local", "link"};
constexpr std::string_view k_path_214[] = {"local", "unlink"};
constexpr std::string_view k_path_215[] = {"local", "import"};
constexpr std::string_view k_path_216[] = {"local", "migrate"};
constexpr std::string_view k_path_217[] = {"bench", "start"};
constexpr std::string_view k_path_218[] = {"bench", "event"};
constexpr std::string_view k_path_219[] = {"bench", "touch"};
constexpr std::string_view k_path_220[] = {"bench", "harvest"};
constexpr std::string_view k_path_221[] = {"bench", "finish"};
constexpr std::string_view k_path_222[] = {"bench", "show"};
constexpr std::string_view k_path_223[] = {"closure", "compute"};
constexpr std::string_view k_path_224[] = {"closure", "show"};
constexpr std::string_view k_path_225[] = {"run", "start"};
constexpr std::string_view k_path_226[] = {"run", "event"};
constexpr std::string_view k_path_227[] = {"run", "finish"};
constexpr std::string_view k_path_228[] = {"run", "show"};
constexpr std::string_view k_path_229[] = {"groups", "recommend"};
constexpr std::string_view k_path_230[] = {"workflow", "list"};
constexpr std::string_view k_path_231[] = {"workflow", "show"};
constexpr std::string_view k_path_232[] = {"workflow", "run"};
constexpr std::string_view k_path_233[] = {"feedback", "triage"};
constexpr std::string_view k_path_234[] = {"plan", "step", "add"};
constexpr std::string_view k_path_235[] = {"plan", "step", "list"};
constexpr std::string_view k_path_236[] = {"plan", "step", "done"};
constexpr std::string_view k_path_237[] = {"plan", "step", "skip"};
constexpr std::string_view k_path_238[] = {"plan", "step", "link"};
constexpr std::string_view k_path_239[] = {"task", "touches", "add"};
constexpr std::string_view k_path_240[] = {"task", "touches", "infer"};
constexpr std::string_view k_path_241[] = {"task", "touches", "list"};
constexpr std::string_view k_path_242[] = {"task", "touches", "remove"};
constexpr std::string_view k_path_243[] = {"workspace", "routing", "build"};
constexpr std::string_view k_path_244[] = {"workspace", "routing", "show"};
constexpr std::string_view k_path_245[] = {"ext", "register", "jira"};
constexpr std::string_view k_path_246[] = {"ext", "register", "github"};
constexpr std::string_view k_path_247[] = {"models", "registry", "list"};
constexpr std::string_view k_path_248[] = {"models", "registry", "add"};
constexpr std::string_view k_path_249[] = {"models", "registry", "update"};
constexpr std::string_view k_path_250[] = {"models", "registry", "remove"};
constexpr std::string_view k_path_251[] = {"models", "registry", "bind"};
constexpr std::string_view k_path_252[] = {"models", "registry", "unbind"};
constexpr std::string_view k_path_253[] = {"models", "registry", "observe"};
constexpr std::string_view k_path_254[] = {"models", "registry", "eligibility"};
constexpr std::string_view k_path_255[] = {"models", "registry", "verify-identity"};
constexpr std::string_view k_path_256[] = {"models", "registry", "export"};
constexpr std::string_view k_path_257[] = {"feedback", "triage", "list"};
constexpr std::string_view k_path_258[] = {"feedback", "triage", "show"};
constexpr std::string_view k_path_259[] = {"feedback", "triage", "set"};

constexpr flag_spec k_flags_0[] = {
    {.name          = "--name",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Project name (defaults to repo dir)"},
    {.name          = "--slug",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Explicit project slug (defaults to a slug derived from the directory name); with --force, targets that "
                      "registration for repoint"},
    {.name          = "--skip-project",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Only init the DB; skip project registration"},
    {.name          = "--allow-no-repo",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Allow initialization outside a git repo"},
    {.name          = "--force",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Overwrite an existing project registration"},
    {.name          = "--json",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Emit machine-readable output"},
};
constexpr flag_spec k_flags_10[] = {
    {.name          = "--to",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Target association slug"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_11[] = {
    {.name          = "--from",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Source association slug"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_15[] = {
    {.name          = "--to",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "<system-slug>:<external-id>"},
    {.name          = "--role",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Link role: mirror, parent, child, reference (default: reference)"},
    {.name          = "--sync",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sync direction: read-only, write-back, two-way (default: read-only)"},
    {.name          = "--propagate",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Propagate feature after linking (M10)"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_16[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Scope for the cross-scope guard (currently informational)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_19[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_20[] = {
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--note", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_23[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_25[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit to a single scope slug"},
    {.name          = "--agents",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Fold in live claim state + next-available-work per plan"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_30[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit to a single scope slug"},
    {.name          = "--all-scopes",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Include every scope"},
    {.name          = "--depth",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "-1",
     .description   = "Max tree depth (-1 = unbounded)"},
    {.name          = "--kind",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single kind"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single status"},
    {.name = "--sort", .kind = "string", .required = false, .list = false, .default_value = {}, .description = "Sort key"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_31[] = {
    {.name          = "--kind",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single kind"},
    {.name          = "--status",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a single status"},
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a scope slug"},
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict to a plan id"},
    {.name = "--limit", .kind = "int", .required = false, .list = false, .default_value = "50", .description = "Max results"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_34[] = {
    {.name          = "--from-github",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Pull source from GitHub issues"},
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--strict", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--threshold",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Similarity threshold, e.g. 0.7"},
    {.name          = "--roadmap",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Path to a roadmap source"},
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--no-status-inference", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--interpret", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--accept-spec",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'"},
    {.name          = "--no-forward-specs",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Skip forward-spec processing entirely"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_35[] = {
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--code-layout", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--treat-as-greenfield", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--treat-as-nongreenfield",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = ""},
    {.name          = "--threshold",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Similarity threshold, e.g. 0.7"},
    {.name = "--literal", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--accept-spec",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Non-interactive forward-spec selection — slug, comma-separated slugs, or 'all'"},
    {.name          = "--no-forward-specs",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Skip forward-spec processing entirely"},
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_39[] = {
    {.name          = "--days",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "30",
     .description   = "Window in days (must be > 0, default 30)."},
    {.name          = "--tail",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "20",
     .description   = "Number of failure-tail rows (must be > 0, default 20)."},
    {.name          = "--json",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Emit stable machine-readable JSON."},
};
constexpr flag_spec k_flags_44[] = {
    {.name          = "--plan",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed initial focus on this plan ID"},
    {.name          = "--task",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed initial focus on this task ID"},
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Seed scope filter"},
    {.name          = "--plain",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Fall back to help/usage instead of launching the cockpit"},
};
constexpr flag_spec k_flags_47[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_48[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_52[] = {
    {.name = "--kind", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_53[] = {
    {.name = "--name", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--kind", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_54[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_55[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_56[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_57[] = {
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_58[] = {
    {.name = "--summary", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = "draft", .description = ""},
    {.name = "--parent", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_59[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_60[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--parent", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--touches", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_61[] = {
    {.name = "--title", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--summary", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--parent", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_62[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_65[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_66[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_67[] = {
    {.name          = "--include-claimed",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Show the claimed bucket in text mode (JSON always includes it)."},
    {.name          = "--include-stale",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Show the stale bucket in text mode (JSON always includes it)."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_68[] = {
    {.name          = "--closure-source",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = "declared",
     .description   = "Rule-2 overlap signal: 'declared' (default, task_touches) or 'derived' (computed closure)."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_69[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_70[] = {
    {.name          = "--plan",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Recompute one plan by id."},
    {.name          = "--all",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Recompute every plan in the DB."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_71[] = {
    {.name          = "--dry-run",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Evaluate and report only; never writes."},
    {.name          = "--check-merge",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Include advisory epic-branch merge roll-up in the output."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_73[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_74[] = {
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--next-action", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--due", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--parent", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--priority", .kind = "int", .required = false, .list = false, .default_value = "100", .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = "true", .description = ""},
    {.name = "--no-auto-promote", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_75[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_76[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_77[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--priority-max", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--touches", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_78[] = {
    {.name = "--title", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--next-action", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--due", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--priority", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--force", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--no-auto-promote", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_79[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_82[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_83[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--force",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Override active-claim guard and flip status anyway."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_84[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_85[] = {
    {.name = "--on", .kind = "int", .required = true, .list = false, .default_value = {}, .description = "Blocking task id"},
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--force",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Override active-claim guard and flip status anyway."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_86[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_87[] = {
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--force",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Override active-claim guard and flip status anyway."},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_89[] = {
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_90[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_93[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_94[] = {
    {.name = "--answer", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_95[] = {
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_96[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--touches", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_97[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_98[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_99[] = {
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--related", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_100[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_103[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_104[] = {
    {.name = "--outcome", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--summary", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_105[] = {
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_106[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--related", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--touches", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_107[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_108[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_109[] = {
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--rationale", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_110[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_111[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_112[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_113[] = {
    {.name = "--by", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_114[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_115[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_118[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_119[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_120[] = {
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--kind", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--from-file", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--source-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = "draft", .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--editor", .kind = "bool", .required = false, .list = false, .default_value = "true", .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_121[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_122[] = {
    {.name = "--kind", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_123[] = {
    {.name = "--title", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--source-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_124[] = {
    {.name = "--no-pull", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_127[] = {
    {.name = "--approve", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--request-changes", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_128[] = {
    {.name = "--relationship", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_129[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--line-start", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--line-end", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--commit-sha", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--text-hash", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--text", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--title", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--tags", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_130[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_131[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--tag", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_132[] = {
    {.name = "--title", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--body", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_133[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_134[] = {
    {.name = "--remove", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_135[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_136[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_137[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_138[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--tag", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_139[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--tag", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_140[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--tag", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_141[] = {
    {.name = "--anchor-path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_142[] = {
    {.name = "--since-days", .kind = "int", .required = false, .list = false, .default_value = "30", .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_143[] = {
    {.name          = "--all",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Validate every workbench tree"},
    {.name          = "--path",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Validate one Markdown file or directory"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_144[] = {
    {.name = "--verbose", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_145[] = {
    {.name = "--verbose", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--filter-mode",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Terminal-status filter: 'failures' (default) or 'all'"},
    {.name          = "--apply-cleanup",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Remove pre-existing FS files for entities this push would have filtered"},
};
constexpr flag_spec k_flags_146[] = {
    {.name = "--verbose", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_147[] = {
    {.name          = "--prefer",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Which side to prefer (fs|db)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_148[] = {
    {.name = "--verbose", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_149[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--filter-mode",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Terminal-status filter: 'failures' (default) or 'all'"},
};
constexpr flag_spec k_flags_150[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--filter-mode",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Terminal-status filter: 'failures' (default) or 'all'"},
};
constexpr flag_spec k_flags_151[] = {
    {.name          = "--dry-run",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Preview only; do not touch disk"},
    {.name          = "--yes",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Discard FS-content drift; remove drifted files anyway"},
    {.name          = "--filter-mode",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Terminal-status filter: 'failures' (default) or 'all'"},
    {.name          = "--all-scopes",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Walk every plan's workbench tree"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_152[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_153[] = {
    {.name = "--system", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_154[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_155[] = {
    {.name = "--editor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_156[] = {
    {.name = "--name", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--slug", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scan", .kind = "int", .required = false, .list = false, .default_value = "1", .description = ""},
    {.name = "--meta-repo", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--no-scan", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--enrich", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_157[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_159[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_161[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_162[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_163[] = {
    {.name          = "--from",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Source local entity ref (kind:id)"},
    {.name          = "--type",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "External issue type, e.g. Epic, Story"},
    {.name          = "--role",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Link role (default: mirror)"},
    {.name          = "--sync",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sync direction (default: two-way)"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_164[] = {
    {.name          = "--from",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Source local entity ref (kind:id, e.g. plan:42 or task:7)"},
    {.name          = "--strategy",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Override GitHub strategy: parent-issue, projects-v2, tracking-issue"},
    {.name          = "--sync",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sync direction for created link: read-only, write-back, two-way"},
    {.name          = "--dry-run",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Preview without contacting the remote system"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_165[] = {
    {.name          = "--system",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "External system slug (defaults to first registered system)"},
    {.name          = "--dry-run",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Preview creation plan without contacting the remote system"},
    {.name          = "--restrategize",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Force fresh strategy detection (M10 engine path)"},
    {.name          = "--yes",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Auto-confirm prompts (M10 engine path)"},
    {.name          = "--verify-counterparts",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Probe remote counterparts (M10 engine path)"},
    {.name          = "--unlink",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Remove links for missing counterparts (requires --verify-counterparts)"},
    {.name          = "--recreate",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Remove and recreate missing counterparts (requires --verify-counterparts)"},
    {.name          = "--github-strategy",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Override GitHub strategy: parent-issue, projects-v2, tracking-issue"},
    {.name          = "--sync",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sync direction for created links: read-only, write-back, two-way"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_166[] = {
    {.name = "--relationship", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_167[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_168[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_169[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_170[] = {
    {.name = "--all", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--system", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_171[] = {
    {.name = "--all", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--system", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_172[] = {
    {.name          = "--entity",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by entity, e.g. task:42"},
    {.name          = "--system",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Filter by system slug"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_173[] = {
    {.name          = "--keep",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Which side to keep (local|remote)"},
    {.name          = "--evidence-token",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Exact token from the approved conflict evidence"},
    {.name          = "--expected-local-updated-at",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Approved local entity updated_at version"},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_177[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_178[] = {
    {.name = "--reason", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_179[] = {
    {.name = "--status", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_181[] = {
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor-session-id", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--model", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_182[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--repo", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--since", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_183[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--summary", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_184[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_185[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--outcome", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_186[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--role", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_187[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--note", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--next-action", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_188[] = {
    {.name = "--kind", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--grep", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--link",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "External link id; switches to link-scoped (external_links + sync_events) form"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_189[] = {
    {.name = "--session", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--task", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--shas", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_190[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_191[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_192[] = {
    {.name = "--threshold", .kind = "int", .required = false, .list = false, .default_value = "90", .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_193[] = {
    {.name          = "--scope",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Limit findings to one association slug"},
    {.name          = "--stale-doing",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "7",
     .description   = "Doing-task age threshold in days"},
    {.name          = "--stale-open",
     .kind          = "int",
     .required      = false,
     .list          = false,
     .default_value = "30",
     .description   = "Open-question age threshold in days"},
};
constexpr flag_spec k_flags_194[] = {
    {.name          = "--vendor",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort vendor; enables evidence-backed ranking"},
    {.name = "--role", .kind = "string", .required = false, .list = false, .default_value = {}, .description = "Cohort role"},
    {.name          = "--tier",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort tier (small|medium|large)"},
    {.name          = "--work-type",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort work type"},
    {.name          = "--complexity",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort complexity (bounded|standard|high-risk)"},
    {.name          = "--project",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort project id"},
    {.name          = "--validation-policy",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort validation policy version"},
    {.name          = "--routing-policy",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Cohort routing policy version"},
    {.name          = "--min-samples",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Minimum samples before a candidate is ranked (default 5)"},
    {.name          = "--quality-floor",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Wilson lower-bound floor (default 0.5)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_195[] = {
    {.name          = "--role",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "planner|spec-reviewer|ingestor|orchestrator|coder|test-coder|reviewer|research|janitor"},
    {.name          = "--task",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Task id (required for task-bound roles)"},
    {.name          = "--plan",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Anchor plan id (pre-task roles)"},
    {.name          = "--fallback-tier",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Configured static fallback tier (default medium)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_196[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_197[] = {
    {.name          = "--limit",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Maximum rows to show (default 50)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_199[] = {
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply-removals", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--format", .kind = "string", .required = false, .list = false, .default_value = "text", .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--strict", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_200[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_201[] = {
    {.name = "--effective", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--raw", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--defaults", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--format", .kind = "string", .required = false, .list = false, .default_value = "text", .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_206[] = {
    {.name = "--system", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--set", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_207[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_208[] = {
    {.name          = "--entity",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Entity ref (kind:id) — task:42, plan:7, scenario:3"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_209[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_210[] = {
    {.name = "--force", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_211[] = {
    {.name = "--system", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--set", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_212[] = {
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_213[] = {
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--vendor", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--reconcile", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_214[] = {
    {.name = "--purge", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_215[] = {
    {.name = "--kind", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--force", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--no-link", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_216[] = {
    {.name = "--dry-run", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_217[] = {
    {.name = "--plan", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--arm", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--base-sha", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--config-hash", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--config-json", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--corpus-repo", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name          = "--task",
     .kind          = "string",
     .required      = false,
     .list          = true,
     .default_value = {},
     .description   = "Limit declared-touch snapshot to this task id (repeatable)."},
};
constexpr flag_spec k_flags_218[] = {
    {.name = "--kind", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--seq", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--payload", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_219[] = {
    {.name = "--task", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--path", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--kind", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_220[] = {
    {.name = "--task", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--worktree", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--base", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--head", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_221[] = {
    {.name = "--status", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_222[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_223[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_224[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_225[] = {
    {.name = "--plan", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--workflow", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_226[] = {
    {.name = "--kind", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--payload", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_227[] = {
    {.name = "--status", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_228[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_229[] = {
    {.name = "--budget", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--solver", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_230[] = {
    {.name = "--local", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_231[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_232[] = {
    {.name          = "--phase",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Phase function to invoke inside the workflow."},
    {.name          = "--args",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "JSON args blob forwarded to planar-execute --args."},
    {.name          = "--worktree",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Worktree directory forwarded to planar-execute --worktree."},
    {.name          = "--sandbox-root",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Sandbox root forwarded to planar-execute --sandbox-root."},
    {.name          = "--local",
     .kind          = "bool",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Restrict resolution to sandbox (local) workflows only."},
};
constexpr flag_spec k_flags_234[] = {
    {.name = "--after", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_235[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_236[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_237[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_238[] = {
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_239[] = {
    {.name = "--path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_240[] = {
    {.name = "--repo", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--apply", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--wide", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_241[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_242[] = {
    {.name = "--path", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_243[] = {
    {.name = "--enrich", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_244[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_245[] = {
    {.name = "--base-url", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--project", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name          = "--auth-env",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "Env var name holding the API token"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_246[] = {
    {.name          = "--project",
     .kind          = "string",
     .required      = true,
     .list          = false,
     .default_value = {},
     .description   = "GitHub repository owner/repo"},
    {.name          = "--auth-env",
     .kind          = "string",
     .required      = false,
     .list          = false,
     .default_value = {},
     .description   = "Env var name holding the token (uses gh-cli if omitted)"},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_247[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_248[] = {
    {.name = "--vendor", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--id", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--order", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--disabled", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_249[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--order", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--disabled", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_250[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_251[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--role", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--tier", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_252[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--role", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--tier", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_253[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--host", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--version", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--availability", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--spawn-verification", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--evidence-ref", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--captured-at", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--expires-at", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_254[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--host", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--role", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--tier", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--now", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--override-supported", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--policy-permits", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_255[] = {
    {.name = "--candidate", .kind = "int", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--actual-vendor", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--actual-id", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_256[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_257[] = {
    {.name = "--plan", .kind = "int", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--severity", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--disposition", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_258[] = {
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};
constexpr flag_spec k_flags_259[] = {
    {.name = "--severity", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--disposition", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--reproduction", .kind = "string", .required = true, .list = false, .default_value = {}, .description = ""},
    {.name = "--duplicate-of", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--evidence", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--scope", .kind = "string", .required = false, .list = false, .default_value = {}, .description = ""},
    {.name = "--json", .kind = "bool", .required = false, .list = false, .default_value = {}, .description = ""},
};

constexpr positional_spec k_pos_10[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_11[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_15[] = {
    {.name = "ref", .required = true, .description = "Entity ref (kind:id)"},
};
constexpr positional_spec k_pos_16[] = {
    {.name = "link-id", .required = true, .description = "External-link id (integer)"},
};
constexpr positional_spec k_pos_19[] = {
    {.name = "task-id", .required = false, .description = ""},
};
constexpr positional_spec k_pos_20[] = {
    {.name = "task-id", .required = false, .description = ""},
};
constexpr positional_spec k_pos_31[] = {
    {.name = "query", .required = true, .description = "FTS5 query string"},
};
constexpr positional_spec k_pos_34[] = {
    {.name = "repo-root", .required = true, .description = ""},
};
constexpr positional_spec k_pos_35[] = {
    {.name = "repo-root", .required = true, .description = ""},
};
constexpr positional_spec k_pos_37[] = {
    {.name = "shell", .required = true, .description = "Shell: bash, zsh, or fish"},
};
constexpr positional_spec k_pos_49[] = {
    {.name = "slug", .required = false, .description = ""},
};
constexpr positional_spec k_pos_53[] = {
    {.name = "slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_54[] = {
    {.name = "slug", .required = true, .description = ""},
    {.name = "repo-path", .required = true, .description = ""},
};
constexpr positional_spec k_pos_55[] = {
    {.name = "slug", .required = true, .description = ""},
    {.name = "repo-path", .required = true, .description = ""},
};
constexpr positional_spec k_pos_56[] = {
    {.name = "slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_58[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_59[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_61[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_62[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_63[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_64[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_65[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_66[] = {
    {.name = "plan-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_67[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_68[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_69[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_71[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_73[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_74[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_75[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_76[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_78[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_79[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_80[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_81[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_82[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_83[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_84[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_85[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_86[] = {
    {.name = "task-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_87[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_89[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_90[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_91[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_92[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_93[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_94[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_95[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_97[] = {
    {.name = "question-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_98[] = {
    {.name = "question-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_99[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_100[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_101[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_102[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_103[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_104[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_105[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_107[] = {
    {.name = "scenario-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_108[] = {
    {.name = "scenario-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_109[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_110[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_112[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_113[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_114[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_115[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_116[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_117[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_118[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_119[] = {
    {.name = "decision-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_120[] = {
    {.name = "title", .required = true, .description = ""},
};
constexpr positional_spec k_pos_121[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_123[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_124[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_125[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_126[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_127[] = {
    {.name = "artifact-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_128[] = {
    {.name = "artifact-id", .required = true, .description = ""},
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_130[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_132[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_133[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_134[] = {
    {.name = "annotation-id", .required = true, .description = ""},
    {.name = "tag", .required = true, .description = ""},
};
constexpr positional_spec k_pos_135[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_136[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_137[] = {
    {.name = "annotation-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_143[] = {
    {.name = "plan", .required = false, .description = ""},
};
constexpr positional_spec k_pos_144[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_145[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_146[] = {
    {.name = "plan", .required = false, .description = ""},
};
constexpr positional_spec k_pos_147[] = {
    {.name = "event-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_148[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_149[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_150[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_151[] = {
    {.name = "plan", .required = false, .description = ""},
};
constexpr positional_spec k_pos_153[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_154[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_155[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_159[] = {
    {.name = "workspace", .required = false, .description = ""},
};
constexpr positional_spec k_pos_162[] = {
    {.name = "slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_163[] = {
    {.name = "system-slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_164[] = {
    {.name = "system", .required = true, .description = ""},
};
constexpr positional_spec k_pos_165[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_166[] = {
    {.name = "from-ref", .required = true, .description = ""},
    {.name = "to-ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_167[] = {
    {.name = "ref", .required = true, .description = ""},
};
constexpr positional_spec k_pos_168[] = {
    {.name = "link-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_169[] = {
    {.name = "link-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_170[] = {
    {.name = "ref", .required = false, .description = "<link-id | kind:id>"},
};
constexpr positional_spec k_pos_171[] = {
    {.name = "ref", .required = false, .description = "<link-id | kind:id>"},
};
constexpr positional_spec k_pos_173[] = {
    {.name = "event-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_174[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_175[] = {
    {.name = "snapshot-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_176[] = {
    {.name = "handoff-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_177[] = {
    {.name = "handoff-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_178[] = {
    {.name = "handoff-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_180[] = {
    {.name = "handoff-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_183[] = {
    {.name = "session-id", .required = false, .description = ""},
};
constexpr positional_spec k_pos_184[] = {
    {.name = "body", .required = true, .description = ""},
};
constexpr positional_spec k_pos_185[] = {
    {.name = "command", .required = true, .description = ""},
};
constexpr positional_spec k_pos_186[] = {
    {.name = "path", .required = true, .description = ""},
};
constexpr positional_spec k_pos_187[] = {
    {.name = "body", .required = false, .description = ""},
};
constexpr positional_spec k_pos_188[] = {
    {.name = "entity-id", .required = false, .description = ""},
};
constexpr positional_spec k_pos_190[] = {
    {.name = "session-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_191[] = {
    {.name = "decision-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_199[] = {
    {.name = "plan", .required = true, .description = ""},
};
constexpr positional_spec k_pos_200[] = {
    {.name = "plan", .required = true, .description = "Plan slug or numeric id"},
};
constexpr positional_spec k_pos_207[] = {
    {.name = "set", .required = true, .description = ""},
    {.name = "system", .required = true, .description = ""},
    {.name = "kind", .required = true, .description = ""},
};
constexpr positional_spec k_pos_208[] = {
    {.name = "set", .required = true, .description = ""},
    {.name = "system", .required = true, .description = ""},
    {.name = "kind", .required = true, .description = ""},
};
constexpr positional_spec k_pos_209[] = {
    {.name = "set", .required = true, .description = ""},
    {.name = "system", .required = true, .description = ""},
    {.name = "kind", .required = true, .description = ""},
};
constexpr positional_spec k_pos_213[] = {
    {.name = "name", .required = false, .description = ""},
};
constexpr positional_spec k_pos_214[] = {
    {.name = "name", .required = true, .description = ""},
};
constexpr positional_spec k_pos_215[] = {
    {.name = "path", .required = true, .description = ""},
};
constexpr positional_spec k_pos_217[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_218[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_219[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_220[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_221[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_222[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_223[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_224[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_226[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_227[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_228[] = {
    {.name = "run-uid", .required = true, .description = ""},
};
constexpr positional_spec k_pos_229[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_231[] = {
    {.name = "name", .required = true, .description = ""},
};
constexpr positional_spec k_pos_232[] = {
    {.name = "name", .required = true, .description = ""},
};
constexpr positional_spec k_pos_234[] = {
    {.name = "plan-id", .required = true, .description = ""},
    {.name = "body", .required = true, .description = ""},
};
constexpr positional_spec k_pos_235[] = {
    {.name = "plan-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_236[] = {
    {.name = "step-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_237[] = {
    {.name = "step-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_238[] = {
    {.name = "step-id", .required = true, .description = ""},
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_239[] = {
    {.name = "task-id", .required = true, .description = ""},
    {.name = "repo-slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_240[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_241[] = {
    {.name = "task-id", .required = true, .description = ""},
};
constexpr positional_spec k_pos_242[] = {
    {.name = "task-id", .required = true, .description = ""},
    {.name = "repo-slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_243[] = {
    {.name = "workspace", .required = false, .description = ""},
};
constexpr positional_spec k_pos_244[] = {
    {.name = "workspace", .required = false, .description = ""},
};
constexpr positional_spec k_pos_245[] = {
    {.name = "slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_246[] = {
    {.name = "slug", .required = true, .description = ""},
};
constexpr positional_spec k_pos_258[] = {
    {.name = "finding", .required = true, .description = ""},
};
constexpr positional_spec k_pos_259[] = {
    {.name = "finding", .required = true, .description = ""},
};

} // namespace

/// @brief Every command node `planar` declares, as a flat list.
///
/// This is the declarative surface the CLI11 tree is built from and the
/// schema catalog is emitted from — one description of the verb set, not
/// two that can drift. `catalog_parity.hpp` diffs it against the Zig
/// oracle's own `schema` output, so a node added here without a handler is
/// caught by dispatch's registration gate rather than shipping as a verb
/// that silently exits 0.
///
/// @return One `node_spec` per command node, in declaration order.
auto surface_nodes() -> std::vector<node_spec> {
  return {
      {.path        = k_path_0,
       .description = "Initialize the Planar database and register the current directory as a project.",
       .flags       = k_flags_0,
       .positionals = {},
       .group       = false},
      {.path = k_path_1,
       .description =
           "Inspect the scope Planar will resolve for the current working\n  directory.\n\n  Plan 153 removed the active scope "
           "stack; scope is now derived from\n  cwd and overridden by passing --scope <slug> to individual verbs.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_2,
       .description = "Manage associations — the many-to-many tags that group repos into\n  named scopes.\n\n  Both 'assoc' and "
                      "'association' are valid subcommand names.\n  User-creatable kinds: org, project, client, personal, "
                      "ad-hoc.\n  Auto-detected kinds (via 'assoc detect'): host, path, lang.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_3,
       .description =
           "Manage plans — the top-level structured intent for a body of work.\n\n  Plans may be hierarchical (--parent) and "
           "contain ordered steps\n  (plan step add).\n  Status lifecycle: draft → active → paused / done / abandoned.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_4,
       .description = "Manage tasks — the discrete units of work.\n\n  Tasks may belong to a plan (--plan) or another task "
                      "(--parent), and\n  carry the next_action field required by resume validate.\n  Status lifecycle: todo → "
                      "doing → done / cancelled; blocked is set\n  via task block.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_5,
       .description = "Manage questions — open uncertainties surfaced during work.\n\n  Status lifecycle: open → answered (via "
                      "'question answer') / wontfix.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_6,
       .description = "Manage test scenarios — verification artifacts tied to specs,\n  plans, or tasks.\n\n  Planar records "
                      "scenarios and their outcomes; it does not execute\n  them.\n  Status lifecycle: draft → ready → verified "
                      "/ failing → retired.\n  Transitions: `scenario verify` (draft→verified via auto-ready, or "
                      "ready→verified),\n  `scenario retire` (any→retired).",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_7,
       .description = "Manage decision records — rationale for choices made during work.\n\n  Status lifecycle: proposed → "
                      "accepted / superseded / withdrawn.\n  Terminal statuses: superseded, withdrawn.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_8,
       .description = "Manage artifacts — durable documents that crystallize from work.\n\n  Kinds: tech_spec, adr, design_note, "
                      "summary, readme, generated,\n  other, product_spec, roadmap, research, getting_started,\n  "
                      "changelog_entry, glossary_term.\n  Status lifecycle: draft → active → superseded/retired.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_9,
       .description =
           "Manage line-anchored annotations on source code.\n\n  Status lifecycle: active → resolved / dismissed / archived.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_10,
       .description = "Promote an entity from its current scope to a named association.\n\n  Valid entity kinds: plan, task, "
                      "question, test_scenario (alias:\n  scenario), artifact, decision.\n\n  Examples:\n    planar promote "
                      "task:42 --to org:acme\n    planar promote plan:7 --to project:planar",
       .flags       = k_flags_10,
       .positionals = k_pos_10,
       .group       = false},
      {.path = k_path_11,
       .description =
           "Reverse a promotion — move an entity back to global personal scope.\n\n  The destination is always global; the "
           "optional --from flag names the\n  source association slug for clarity. Association-to-association\n  transitions go "
           "through promote.\n\n  Example:\n    planar demote task:42 --from project:planar",
       .flags       = k_flags_11,
       .positionals = k_pos_11,
       .group       = false},
      {.path = k_path_12,
       .description =
           "Manage the bidirectional sync surface between the workbench\n  filesystem and the Planar database.\n\n  The "
           "workbench root resolution order (highest to lowest priority):\n    1. $PLANAR_WORKBENCH_ROOT env var\n    2. "
           "workbench.root in $PLANAR_CONFIG_PATH or ~/.planar/config.toml\n    3. Default: ~/.planar/workbench/",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_13,
       .description = "Workspace administration.\n\n  A workspace is identified by an associations row of kind=org. Each\n  "
                      "workspace owns a state directory under\n  ${PLANAR_HOME:-~/.planar}/workspaces/<org_id>/ holding the "
                      "canonical\n  AGENTS.md surface for the org.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_14,
       .description = "Register and interact with external systems on the operational plane.\n\n  Sub-commands: register, list, "
                      "test, create, propagate.\n  Currently supported systems: Jira, GitHub Issues, GitHub Projects.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_15,
       .description = "Manually record an external_links row linking a local entity to\n  an already-existing external ticket. "
                      "Use this when the external\n  ticket was created outside of 'ext create'. Does not push any data\n  to "
                      "the external system.\n\n  <kind:id> is a local entity reference, e.g. task:42, plan:7.",
       .flags       = k_flags_15,
       .positionals = k_pos_15,
       .group       = false},
      {.path = k_path_16,
       .description =
           "Remove an external_links row by its link id.\n\n  Associated sync_events rows are detached by setting link_id to "
           "null\n  rather than cascade-deleted; they are no longer reachable through\n  the deleted link's audit trail.",
       .flags       = k_flags_16,
       .positionals = k_pos_16,
       .group       = false},
      {.path = k_path_17,
       .description =
           "Manage internal cross-cutting entity_links relationships.\n\n  Entity links record typed relationships between any "
           "two Planar\n  entities (e.g. a task cites an artifact, a plan blocks another\n  plan). This domain is distinct from "
           "the top-level link/unlink\n  commands, which operate on external-system ticket linkage.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_18,
       .description = "Pull and push data between the local plane and external systems.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_19,
       .description =
           "Produce a structured 8-section resume packet for the specified\n  task.\n\n  The packet contains:\n    1. Identity   "
           "    — task id, plan id, title, scope\n    2. State          — status, next_action, last action\n    3. Plan position "
           " — parent plan, completed/current/remaining steps\n    4. Operational    — external_links for the task; refreshed if "
           "stale\n    5. Recent activity — session entries from recent sessions\n    6. Decisions and questions\n    7. Linked "
           "artifacts\n    8. Audit footer   — previous session vendor and timestamp, plus\n                        the active "
           "claim's worktree path (when held)\n                        so the resumer can prepend `cd <path>`",
       .flags       = k_flags_19,
       .positionals = k_pos_19,
       .group       = true},
      {.path = k_path_20,
       .description =
           "Capture a context snapshot for the current session and atomically:\n    1. Insert a context_snapshots row.\n    2. "
           "Insert a handoffs row with status='pending'.\n    3. Validate the handoff (pending → validated, validated_at "
           "set).\n\n  Subcommands manage the handoff lifecycle: create / validate /\n  consume / abandon / list / show.",
       .flags       = k_flags_20,
       .positionals = k_pos_20,
       .group       = true},
      {.path        = k_path_21,
       .description = "Capture commands manage explicit session management and context\n  capture.\n\n  Automatic capture "
                      "happens on every write command; use these\n  subcommands for explicit session management, narrative "
                      "notes,\n  command history, and snapshots.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_22,
       .description = "Cross-plane audit trail commands.\n\n  Subcommands inspect external-link history, query attributed "
                      "session\n  commits, recompute decision publication targets, render session\n  timelines, and walk the "
                      "full audit trail for any external link.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_23,
       .description =
           "Check database reachability, schema version currency, SQLite\n  integrity, in-flight task resumability, pending "
           "handoff staleness,\n  and manifest-owned installed projection freshness. This command is\n  read-only; recovery "
           "commands are reported but never run.\n\n  Exit codes:\n    0  all checks pass\n    1  degraded (in-flight tasks not "
           "resumable, stale handoffs, stale\n       or missing managed projections, integrity errors, etc.)",
       .flags       = k_flags_23,
       .positionals = {},
       .group       = true},
      {.path = k_path_24,
       .description =
           "Probe the supported provider CLIs (claude, codex) for\n  installed-state + version, and report their curated model\n "
           " catalogs and the default role→tier→model routing.\n\n  The provider CLIs do not expose a machine-readable model "
           "list,\n  so the per-vendor model catalog is curated in-repo; discovery\n  confirms which CLIs are callable on this "
           "machine.\n\n  Subcommands:\n    list       Probe + print (read-only).\n    refresh    Probe + print, and write the "
           "cache to\n               ${PLANAR_HOME:-~/.planar}/models/catalog.json.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_25,
       .description =
           "Roll-up of in-flight plans in the current scope.\n\n  --agents folds in the live claim state from agent_work_claims "
           "—\n  active claims, stale claims, and the per-plan 'next available'\n  task list. Without --agents the dashboard is "
           "a plain plan summary.\n\n  This is the operator's read surface for agent activity; the\n  `planar agent` subcommand "
           "namespace does not exist by design.\n  See `planar-agent` for the ritual (claim/heartbeat/complete) and\n  "
           "`planar-watch` for the live streaming view.",
       .flags       = k_flags_25,
       .positionals = {},
       .group       = false},
      {.path = k_path_26,
       .description =
           "Commands for the planning pipeline spec surface.\n\n  'spec ingest' decomposes workbench planning documents into a\n "
           " structured task graph in the database.\n  'spec draft' generates initial spec artifacts from a goal statement.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_27,
       .description = "Commands for inspecting test-spec coverage of a plan's tasks.\n\n  'test-spec status' prints a "
                      "per-milestone breakdown of which tasks\n  have verifying scenarios. This is a read-only complement to "
                      "the\n  ingest-time coverage gate (see `planar spec ingest --strict`).",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_28,
       .description =
           "Read, inspect, and validate the Planar configuration file.\n\n  The configuration file lives at "
           "~/.planar/config.toml by default.\n  Set $PLANAR_CONFIG_PATH to use a different path.\n  Resolution order (highest "
           "to lowest priority):\n    1. Explicit --config-path flag\n    2. $PLANAR_CONFIG_PATH\n    3. ~/.planar/config.toml",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_29,
       .description =
           "Manage the template plane: list available templates, show their\n  raw JSON, render them against a DB entity (dry "
           "run), validate\n  syntax, initialise the default set on disk, and print resolution\n  paths.\n\n  Templates resolve "
           "via a three-level fallback chain:\n    1. ~/.planar/templates/<kind>/<slug>.json (operator overrides)\n    2. "
           "~/.planar/templates/defaults/<kind>/<slug>.json (default copies)\n    3. templates/defaults/<kind>/<slug>.json "
           "(embedded in the binary)",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_30,
       .description = "Render a hierarchical view of Planar entities for one or all\n  scopes.\n\n  Walks plans (via "
                      "parent_plan_id), tasks (via plan_id and\n  parent_task_id), and entity_links(derives-from) to gather\n  "
                      "artifacts, decisions, scenarios, and questions linked to each plan.",
       .flags       = k_flags_30,
       .positionals = {},
       .group       = false},
      {.path        = k_path_31,
       .description = "Run a full-text search across every searchable entity kind.\n\n  Queries are passed to SQLite's FTS5 "
                      "MATCH operator directly.\n  Multi-word queries are AND'd unless the operator is given\n  explicitly (OR, "
                      "NOT, NEAR, \"phrase\"). Tokens are unicode61-folded\n  (case-insensitive, diacritic-stripped).",
       .flags       = k_flags_31,
       .positionals = k_pos_31,
       .group       = false},
      {.path        = k_path_32,
       .description = "Manage the operator's local sandbox for personal skills and agents.\n\n  Authors a single source file per "
                      "skill or agent under\n  ~/.planar/local/ and creates per-vendor symlinks (with copy\n  fallback) into "
                      "each vendor's install directory.\n  Edits to the source file propagate immediately to every vendor.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_33,
       .description = "The unified skill source tree under skills/src/ is rendered by the\n  external scriptorium binary (plan "
                      "918). Planar no longer renders vendor\n  projections nor tracks their install-drift in-band; use "
                      "`scriptorium\n  check`/`scriptorium status` instead. This command has no subcommands.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_34,
       .description = "import translates the planning artefacts of an existing\n  repository into Planar's data model. It "
                      "discovers\n  tech specs, roadmap milestones, ADRs, and backlog files,\n  infers completion status from "
                      "checkbox state and git history,\n  and produces an ImportPlan for review before committing.",
       .flags       = k_flags_34,
       .positionals = k_pos_34,
       .group       = false},
      {.path        = k_path_35,
       .description = "synthesize reads a repository's existing planning docs, source\n  code, and git history AS INPUT for an "
                      "LLM synthesis pass. It\n  produces fresh product-spec / tech-spec / roadmap artifacts (NOT a\n  verbatim "
                      "transcription) and proposes them via the same workbench\n  pipeline as the planner agent.",
       .flags       = k_flags_35,
       .positionals = k_pos_35,
       .group       = false},
      {.path        = k_path_36,
       .description = "Print the planar version, commit, and zig runtime.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_37,
       .description = "Generate the autocompletion script for the specified shell.",
       .flags       = {},
       .positionals = k_pos_37,
       .group       = false},
      {.path        = k_path_38,
       .description = "Print the full command tree as a JSON catalog (flags, aliases, positionals).",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_39,
       .description = "Reads the cli_invocations capture log and the always-on observability\ntables (agent_actions, "
                      "sync_events, agent_work_claims, handoffs) and\nrenders a diagnostic bundle.\n\nInvocation and failure "
                      "sections render \"logging disabled\" when\n[introspection].cli_log is off; the always-on sections "
                      "(actions, sync,\nclaims, claim failure categories, handoffs, health) render normally in\neither "
                      "case.\n\nThe bundle is structurally redacted: queries select only counts,\ncategories, verb paths, "
                      "statuses, and timestamps — never entity text.\n\nExit codes:\n  0   bundle rendered successfully.\n  2   "
                      "invalid flag value (--days or --tail must be a positive integer).\n  1   database error.",
       .flags       = k_flags_39,
       .positionals = {},
       .group       = false},
      {.path        = k_path_40,
       .description = "Record measurement-rig data for the vertical-slice decomposition experiment.\n\n  Arms: strict, "
                      "eligibility, grouped (or any free-text pilot value).\n  Statuses: running, completed, aborted, error.\n  "
                      "Touch kinds: declared, actual.\n\n  Workflow: bench start → bench event (repeat) → bench touch (repeat)\n "
                      "           → bench harvest → bench finish → bench show --json.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_41,
       .description = "Compute the *derived* closure of a task — the symbols it must hold\nresident, computed by static analysis "
                      "from the task's declared seed\npaths (task_touch_paths), partitioned by role:\n\n  modify     — the "
                      "seed's own edited symbols.\n  reference  — the interfaces the seed depends on.\n  transitive — deeper "
                      "hops (stored, but excluded from the effective\n               closure by default).\n\n  Workflow: closure "
                      "compute <task-id> → closure show <task-id> --json.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_42,
       .description = "Record operational run traces emitted by workflows.\n\n  Arm defaults to 'op' (or the workflow name when "
                      "--workflow is given).\n  Statuses: running, completed, aborted, error.\n\n  Workflow: run start → run "
                      "event (repeat) → run finish → run show --json.\n\n  See `planar bench` for the measurement-rig surface "
                      "(strict/eligibility/\n  grouped arms, declared/actual touch tracking, git-diff harvest).",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_43,
       .description =
           "Form **slices** — groups of a plan's open (todo) tasks that share a\ncontext window — by minimizing the duplicated "
           "closure across slices,\nsubject to a per-slice token budget. Read-only sibling to\n`plan recommend-strategy`: it "
           "reports a recommendation, writing nothing.\n\nEach slice reports its member task ids, its unioned effective "
           "closure\n(the distinct symbols the slice must hold resident, role modify ∪\nreference), and that union's token cost. "
           "No slice's cost exceeds the\nbudget, and the slice-DAG induced by the task `blocks` dependencies is\nalways "
           "schedulable (no slice is grouped across a dependency violation).\n\n  --solver greedy|mtkahypar  (default greedy) "
           "selects the partitioner.\n  `mtkahypar` is the optional external hypergraph solver: when its binary\n  is absent or "
           "fails, the verb degrades to greedy and reports\n  `optimal_available:false` (it never errors on a missing optional "
           "dep).\n\n  Workflow: closure compute <task> (per task) → groups recommend <plan>.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_44,
       .description =
           "Launch the interactive Planar cockpit.\n\n  Equivalent to invoking `planar` with no verb on a terminal. Use\n  "
           "`planar explore` when you want to force-launch the cockpit by name,\n  or from a context where bare-invocation "
           "detection may not fire.\n\n  --plan, --task, and --scope seed the initial focus.\n\n  Falls back to this help text "
           "when stdout is not a TTY, when TERM=dumb,\n  when PLANAR_NO_TUI is set, or when --plain is passed.",
       .flags       = k_flags_44,
       .positionals = {},
       .group       = false},
      {.path        = k_path_45,
       .description = "Enumerate, inspect, and invoke shipped and sandbox Lua workflows.\n\n  Shipped workflows live at "
                      "$PLANAR_HOME/workflows/ (default\n  ~/.planar/workflows/).  Sandbox workflows live at\n  "
                      "~/.planar/local/workflows/ and are marked `local`.\n\n  These commands are READ-ONLY w.r.t. SQLite.  "
                      "`run` delegates\n  execution to `planar-execute` and forwards its output + exit code.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path = k_path_46, .description = "Manage structured feedback.", .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_47,
       .description = "Show the cwd-derived scope (and any --scope override).",
       .flags       = k_flags_47,
       .positionals = {},
       .group       = false},
      {.path        = k_path_48,
       .description = "Suggest scope associations based on cwd.",
       .flags       = k_flags_48,
       .positionals = {},
       .group       = false},
      {.path        = k_path_49,
       .description = "Removed in plan 153 M5 — see `planar scope show`.",
       .flags       = {},
       .positionals = k_pos_49,
       .group       = false},
      {.path        = k_path_50,
       .description = "Removed in plan 153 M5 — see `planar scope show`.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path        = k_path_51,
       .description = "Removed in plan 153 M5 — see `planar scope show`.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path = k_path_52, .description = "List all known associations.", .flags = k_flags_52, .positionals = {}, .group = false},
      {.path        = k_path_53,
       .description = "Create a new association.",
       .flags       = k_flags_53,
       .positionals = k_pos_53,
       .group       = false},
      {.path        = k_path_54,
       .description = "Add a repo to an association.",
       .flags       = k_flags_54,
       .positionals = k_pos_54,
       .group       = false},
      {.path        = k_path_55,
       .description = "Remove a repo from an association.",
       .flags       = k_flags_55,
       .positionals = k_pos_55,
       .group       = false},
      {.path        = k_path_56,
       .description = "List all project members of an association.",
       .flags       = k_flags_56,
       .positionals = k_pos_56,
       .group       = false},
      {.path        = k_path_57,
       .description = "Propose (or apply) auto-detected associations for the current directory.",
       .flags       = k_flags_57,
       .positionals = {},
       .group       = false},
      {.path = k_path_58, .description = "Create a new plan.", .flags = k_flags_58, .positionals = k_pos_58, .group = false},
      {.path        = k_path_59,
       .description = "Show a plan's details, steps, and child plans.",
       .flags       = k_flags_59,
       .positionals = k_pos_59,
       .group       = false},
      {.path = k_path_60, .description = "List plans.", .flags = k_flags_60, .positionals = {}, .group = false},
      {.path        = k_path_61,
       .description = "Update mutable fields on a plan.",
       .flags       = k_flags_61,
       .positionals = k_pos_61,
       .group       = false},
      {.path        = k_path_62,
       .description = "Edit a plan in $EDITOR (editor-first flow).",
       .flags       = k_flags_62,
       .positionals = k_pos_62,
       .group       = false},
      {.path = k_path_63, .description = "View a plan's workbench file.", .flags = {}, .positionals = k_pos_63, .group = false},
      {.path        = k_path_64,
       .description = "Diff plan against database version.",
       .flags       = {},
       .positionals = k_pos_64,
       .group       = false},
      {.path        = k_path_65,
       .description = "Reviewer entry point for plan diff.",
       .flags       = k_flags_65,
       .positionals = k_pos_65,
       .group       = false},
      {.path        = k_path_66,
       .description = "Create an entity link from a plan to another entity.",
       .flags       = k_flags_66,
       .positionals = k_pos_66,
       .group       = false},
      {.path        = k_path_67,
       .description = "Bucketed claim-aware view of next work on a plan.\n\n  Buckets:\n    available  task is todo (or doing "
                      "without an active claim)\n               and ready to be pulled\n    claimed    task has an active "
                      "unexpired claim\n    stale      task has a stale claim (reconcile or lease-expired)\n    blocked    task "
                      "status is blocked\n\n  Without --include-claimed / --include-stale the text rendering\n  shows only the "
                      "available + blocked buckets — the JSON shape always\n  carries every bucket.",
       .flags       = k_flags_67,
       .positionals = k_pos_67,
       .group       = false},
      {.path = k_path_68,
       .description =
           "Recommend an execution strategy for a plan's open (todo) tasks.\n\n  Applies the six parallel-eligibility rules "
           "(decision 370) and\n  reports the parallel-eligible subset plus the serialized remainder\n  with per-task exclusion "
           "reasons:\n    1. no blocked_by chain to a not-done task\n    2. disjoint task_touches (empty touches = "
           "touches-everything)\n    3. no schema migration touched\n    4. no singleton authoritative file touched\n    5. no "
           "open question linked\n    6. no proposed decision linked\n\n  --closure-source selects the signal rule 2's overlap "
           "test reads\n  (decision D4): 'declared' (default) uses the declared task_touches and\n  is byte-for-byte the pre-D4 "
           "behavior; 'derived' uses the computed\n  symbol-level closure (closures table) so two tasks overlap when their\n  "
           "derived closures share a symbol even when their declared files differ.\n\n  READ-ONLY: computes and reports; writes "
           "nothing. fan_out_available\n  is true when >= 2 tasks are eligible.",
       .flags       = k_flags_68,
       .positionals = k_pos_68,
       .group       = false},
      {.path = k_path_69,
       .description =
           "Report the declared-vs-derived closure divergence for a plan's open tasks.\n\n  For every unordered pair of open "
           "(todo) tasks, compares whether the two\n  tasks overlap under the DECLARED touch set vs. the DERIVED closure set.\n  "
           "A pair whose verdict differs between sources is a FLIP — the two sources\n  disagree about whether those tasks can "
           "run in parallel.\n\n  Jaccard distance = flips / |declared_overlaps ∪ derived_overlaps|.\n  0.0 = sources agree on "
           "every pair; 1.0 = no overlapping pair in common.\n\n  READ-ONLY: computes and reports; writes nothing.",
       .flags       = k_flags_69,
       .positionals = k_pos_69,
       .group       = false},
      {.path        = k_path_70,
       .description = "Recompute a plan's roll-up status (--plan <id> or --all).",
       .flags       = k_flags_70,
       .positionals = {},
       .group       = false},
      {.path        = k_path_71,
       .description = "Evaluate the DB-hard gate (all tasks terminal, all descendants terminal, no live claims)\n  and advisory "
                      "git-evidence for a plan. In apply mode (no --dry-run), marks the plan\n  done when the hard gate passes. "
                      "Cancelled tasks are terminal — they do not block.\n\n  Hard gate failures produce a non-zero exit in both "
                      "dry-run and apply modes.\n\n  --check-merge adds an advisory epic-branch merge roll-up: for each "
                      "contributing\n  branch from agent_work_claims, reports how many are merged to the target branch.\n  Never "
                      "blocks; absent branches are inconclusive.",
       .flags       = k_flags_71,
       .positionals = k_pos_71,
       .group       = false},
      {.path = k_path_72, .description = "Manage plan steps.", .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_73,
       .description = "Emit the anchor plan's full subtree (child plans + tasks) in\n  dependency-topological order (anchor → "
                      "child plans → tasks).\n\n  READ-ONLY: queries and reports; writes nothing.",
       .flags       = k_flags_73,
       .positionals = k_pos_73,
       .group       = false},
      {.path = k_path_74, .description = "Create a new task.", .flags = k_flags_74, .positionals = k_pos_74, .group = false},
      {.path = k_path_75, .description = "Show full task details.", .flags = k_flags_75, .positionals = k_pos_75, .group = false},
      {.path        = k_path_76,
       .description = "Compile the authoritative current routing packet for a task.",
       .flags       = k_flags_76,
       .positionals = k_pos_76,
       .group       = false},
      {.path = k_path_77, .description = "List tasks.", .flags = k_flags_77, .positionals = {}, .group = false},
      {.path        = k_path_78,
       .description = "Update mutable fields on a task.",
       .flags       = k_flags_78,
       .positionals = k_pos_78,
       .group       = false},
      {.path        = k_path_79,
       .description = "Edit a task in $EDITOR (editor-first flow).",
       .flags       = k_flags_79,
       .positionals = k_pos_79,
       .group       = false},
      {.path = k_path_80, .description = "View task's workbench file.", .flags = {}, .positionals = k_pos_80, .group = false},
      {.path        = k_path_81,
       .description = "Diff task against its database-stored version.",
       .flags       = {},
       .positionals = k_pos_81,
       .group       = false},
      {.path        = k_path_82,
       .description = "Reviewer entry point for task diff.",
       .flags       = k_flags_82,
       .positionals = k_pos_82,
       .group       = false},
      {.path        = k_path_83,
       .description = "Mark a task as done (single-arg form; Go supports variadic).",
       .flags       = k_flags_83,
       .positionals = k_pos_83,
       .group       = false},
      {.path        = k_path_84,
       .description = "Cancel a task (single-arg form; Go supports variadic).",
       .flags       = k_flags_84,
       .positionals = k_pos_84,
       .group       = false},
      {.path        = k_path_85,
       .description = "Mark a task as blocked and record the blocking relationship.",
       .flags       = k_flags_85,
       .positionals = k_pos_85,
       .group       = false},
      {.path        = k_path_86,
       .description = "Create an entity link from a task to another entity.",
       .flags       = k_flags_86,
       .positionals = k_pos_86,
       .group       = false},
      {.path        = k_path_87,
       .description = "Reopen a done or cancelled task with an audit-trail entry.",
       .flags       = k_flags_87,
       .positionals = k_pos_87,
       .group       = false},
      {.path = k_path_88, .description = "Manage repo-touches links on a task.", .flags = {}, .positionals = {}, .group = true},
      {.path = k_path_89, .description = "Create a new question.", .flags = k_flags_89, .positionals = k_pos_89, .group = false},
      {.path        = k_path_90,
       .description = "Edit a question in $EDITOR (editor-first flow).",
       .flags       = k_flags_90,
       .positionals = k_pos_90,
       .group       = false},
      {.path = k_path_91, .description = "View question's workbench file.", .flags = {}, .positionals = k_pos_91, .group = false},
      {.path        = k_path_92,
       .description = "Diff question against database version.",
       .flags       = {},
       .positionals = k_pos_92,
       .group       = false},
      {.path        = k_path_93,
       .description = "Reviewer entry point for question diff.",
       .flags       = k_flags_93,
       .positionals = k_pos_93,
       .group       = false},
      {.path        = k_path_94,
       .description = "Record an answer to a question.",
       .flags       = k_flags_94,
       .positionals = k_pos_94,
       .group       = false},
      {.path        = k_path_95,
       .description = "Mark a question as wontfix.",
       .flags       = k_flags_95,
       .positionals = k_pos_95,
       .group       = false},
      {.path = k_path_96, .description = "List questions.", .flags = k_flags_96, .positionals = {}, .group = false},
      {.path        = k_path_97,
       .description = "Show a question's details.",
       .flags       = k_flags_97,
       .positionals = k_pos_97,
       .group       = false},
      {.path        = k_path_98,
       .description = "Create an entity link from a question to another entity.",
       .flags       = k_flags_98,
       .positionals = k_pos_98,
       .group       = false},
      {.path        = k_path_99,
       .description = "Create a new test scenario.",
       .flags       = k_flags_99,
       .positionals = k_pos_99,
       .group       = false},
      {.path        = k_path_100,
       .description = "Edit a scenario in $EDITOR (editor-first flow).",
       .flags       = k_flags_100,
       .positionals = k_pos_100,
       .group       = false},
      {.path        = k_path_101,
       .description = "View scenario's workbench file.",
       .flags       = {},
       .positionals = k_pos_101,
       .group       = false},
      {.path        = k_path_102,
       .description = "Diff scenario against database version.",
       .flags       = {},
       .positionals = k_pos_102,
       .group       = false},
      {.path        = k_path_103,
       .description = "Reviewer entry point for scenario diff.",
       .flags       = k_flags_103,
       .positionals = k_pos_103,
       .group       = false},
      {.path        = k_path_104,
       .description = "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass).",
       .flags       = k_flags_104,
       .positionals = k_pos_104,
       .group       = false},
      {.path        = k_path_105,
       .description = "Mark a scenario as retired.",
       .flags       = k_flags_105,
       .positionals = k_pos_105,
       .group       = false},
      {.path = k_path_106, .description = "List scenarios.", .flags = k_flags_106, .positionals = {}, .group = false},
      {.path        = k_path_107,
       .description = "Show a scenario's details.",
       .flags       = k_flags_107,
       .positionals = k_pos_107,
       .group       = false},
      {.path        = k_path_108,
       .description = "Create an entity link from a scenario to another entity.",
       .flags       = k_flags_108,
       .positionals = k_pos_108,
       .group       = false},
      {.path        = k_path_109,
       .description = "Create a new decision record.",
       .flags       = k_flags_109,
       .positionals = k_pos_109,
       .group       = false},
      {.path        = k_path_110,
       .description = "Show a decision's details.",
       .flags       = k_flags_110,
       .positionals = k_pos_110,
       .group       = false},
      {.path = k_path_111, .description = "List decisions.", .flags = k_flags_111, .positionals = {}, .group = false},
      {.path        = k_path_112,
       .description = "Accept a proposed decision.",
       .flags       = k_flags_112,
       .positionals = k_pos_112,
       .group       = false},
      {.path        = k_path_113,
       .description = "Mark a decision as superseded by a newer decision.",
       .flags       = k_flags_113,
       .positionals = k_pos_113,
       .group       = false},
      {.path = k_path_114, .description = "Withdraw a decision.", .flags = k_flags_114, .positionals = k_pos_114, .group = false},
      {.path        = k_path_115,
       .description = "Edit a decision in $EDITOR (editor-first flow).",
       .flags       = k_flags_115,
       .positionals = k_pos_115,
       .group       = false},
      {.path        = k_path_116,
       .description = "View decision's workbench file.",
       .flags       = {},
       .positionals = k_pos_116,
       .group       = false},
      {.path        = k_path_117,
       .description = "Diff decision against database version.",
       .flags       = {},
       .positionals = k_pos_117,
       .group       = false},
      {.path        = k_path_118,
       .description = "Reviewer entry point for decision diff.",
       .flags       = k_flags_118,
       .positionals = k_pos_118,
       .group       = false},
      {.path        = k_path_119,
       .description = "Create an entity link from a decision to another entity.",
       .flags       = k_flags_119,
       .positionals = k_pos_119,
       .group       = false},
      {.path        = k_path_120,
       .description = "Register a new artifact.",
       .flags       = k_flags_120,
       .positionals = k_pos_120,
       .group       = false},
      {.path        = k_path_121,
       .description = "Show an artifact's metadata and body.",
       .flags       = k_flags_121,
       .positionals = k_pos_121,
       .group       = false},
      {.path = k_path_122, .description = "List artifacts.", .flags = k_flags_122, .positionals = {}, .group = false},
      {.path        = k_path_123,
       .description = "Update mutable fields on an artifact.",
       .flags       = k_flags_123,
       .positionals = k_pos_123,
       .group       = false},
      {.path        = k_path_124,
       .description = "Edit an artifact in $EDITOR (editor-first flow).",
       .flags       = k_flags_124,
       .positionals = k_pos_124,
       .group       = false},
      {.path        = k_path_125,
       .description = "View the artifact's workbench file in $PAGER.",
       .flags       = {},
       .positionals = k_pos_125,
       .group       = false},
      {.path        = k_path_126,
       .description = "Show a unified diff between the DB's artifact content and the workbench file.",
       .flags       = {},
       .positionals = k_pos_126,
       .group       = false},
      {.path        = k_path_127,
       .description = "Reviewer entry point for artifact diff.",
       .flags       = k_flags_127,
       .positionals = k_pos_127,
       .group       = false},
      {.path        = k_path_128,
       .description = "Create an entity link from an artifact to another entity.",
       .flags       = k_flags_128,
       .positionals = k_pos_128,
       .group       = false},
      {.path = k_path_129, .description = "Create a new annotation.", .flags = k_flags_129, .positionals = {}, .group = false},
      {.path = k_path_130, .description = "Show an annotation.", .flags = k_flags_130, .positionals = k_pos_130, .group = false},
      {.path = k_path_131, .description = "List annotations.", .flags = k_flags_131, .positionals = {}, .group = false},
      {.path        = k_path_132,
       .description = "Update an annotation.",
       .flags       = k_flags_132,
       .positionals = k_pos_132,
       .group       = false},
      {.path        = k_path_133,
       .description = "Remove an annotation.",
       .flags       = k_flags_133,
       .positionals = k_pos_133,
       .group       = false},
      {.path        = k_path_134,
       .description = "Add or remove a tag on an annotation.",
       .flags       = k_flags_134,
       .positionals = k_pos_134,
       .group       = false},
      {.path        = k_path_135,
       .description = "Mark an annotation as resolved.",
       .flags       = k_flags_135,
       .positionals = k_pos_135,
       .group       = false},
      {.path        = k_path_136,
       .description = "Dismiss an annotation.",
       .flags       = k_flags_136,
       .positionals = k_pos_136,
       .group       = false},
      {.path        = k_path_137,
       .description = "Archive an annotation.",
       .flags       = k_flags_137,
       .positionals = k_pos_137,
       .group       = false},
      {.path        = k_path_138,
       .description = "Resolve every active annotation matching the filter.",
       .flags       = k_flags_138,
       .positionals = {},
       .group       = false},
      {.path        = k_path_139,
       .description = "Dismiss every active annotation matching the filter.",
       .flags       = k_flags_139,
       .positionals = {},
       .group       = false},
      {.path        = k_path_140,
       .description = "Archive every annotation matching the filter (including non-active rows).",
       .flags       = k_flags_140,
       .positionals = {},
       .group       = false},
      {.path        = k_path_141,
       .description = "Verify annotation anchors against workspace state.",
       .flags       = k_flags_141,
       .positionals = {},
       .group       = false},
      {.path        = k_path_142,
       .description = "Sweep stale annotations (resolved/dismissed older than --since-days).",
       .flags       = k_flags_142,
       .positionals = {},
       .group       = false},
      {.path        = k_path_143,
       .description = "Validate workbench Markdown frontmatter without syncing.",
       .flags       = k_flags_143,
       .positionals = k_pos_143,
       .group       = false},
      {.path        = k_path_144,
       .description = "Apply FS→DB changes; report DB→FS drift.",
       .flags       = k_flags_144,
       .positionals = k_pos_144,
       .group       = false},
      {.path        = k_path_145,
       .description = "Apply DB→FS changes atomically; report FS→DB drift.",
       .flags       = k_flags_145,
       .positionals = k_pos_145,
       .group       = false},
      {.path        = k_path_146,
       .description = "Show drift and conflicts without writing.",
       .flags       = k_flags_146,
       .positionals = k_pos_146,
       .group       = false},
      {.path        = k_path_147,
       .description = "Settle a sync conflict by choosing FS or DB.",
       .flags       = k_flags_147,
       .positionals = k_pos_147,
       .group       = false},
      {.path        = k_path_148,
       .description = "Atomically apply FS and DB changes via a unified sync.",
       .flags       = k_flags_148,
       .positionals = k_pos_148,
       .group       = false},
      {.path        = k_path_149,
       .description = "Archive a feature's workbench filesystem tree.",
       .flags       = k_flags_149,
       .positionals = k_pos_149,
       .group       = false},
      {.path        = k_path_150,
       .description = "Restore an archived feature's workbench tree.",
       .flags       = k_flags_150,
       .positionals = k_pos_150,
       .group       = false},
      {.path        = k_path_151,
       .description = "Remove FS files whose backing entity is terminal in the DB.",
       .flags       = k_flags_151,
       .positionals = k_pos_151,
       .group       = false},
      {.path        = k_path_152,
       .description = "List features with workbench trees.",
       .flags       = k_flags_152,
       .positionals = {},
       .group       = false},
      {.path        = k_path_153,
       .description = "Render and push workbench files to external system.",
       .flags       = k_flags_153,
       .positionals = k_pos_153,
       .group       = false},
      {.path        = k_path_154,
       .description = "Parse Open questions from top-level workbench specs (read-only).",
       .flags       = k_flags_154,
       .positionals = k_pos_154,
       .group       = false},
      {.path        = k_path_155,
       .description = "Edit a feature's workbench files in $EDITOR.",
       .flags       = k_flags_155,
       .positionals = k_pos_155,
       .group       = false},
      {.path        = k_path_156,
       .description = "Initialize a workspace (org-level association).",
       .flags       = k_flags_156,
       .positionals = {},
       .group       = false},
      {.path        = k_path_157,
       .description = "Scan and fix workspace registration and state consistency.",
       .flags       = k_flags_157,
       .positionals = {},
       .group       = false},
      {.path = k_path_158, .description = "Manage workspace routing table.", .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_159,
       .description = "Regenerate AGENTS.md from current state.",
       .flags       = k_flags_159,
       .positionals = k_pos_159,
       .group       = false},
      {.path = k_path_160, .description = "Register an external system.", .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_161,
       .description = "List registered external systems.",
       .flags       = k_flags_161,
       .positionals = {},
       .group       = false},
      {.path        = k_path_162,
       .description = "Test connection to an external system.",
       .flags       = k_flags_162,
       .positionals = k_pos_162,
       .group       = false},
      {.path        = k_path_163,
       .description = "Create an external counterpart for a local entity.",
       .flags       = k_flags_163,
       .positionals = k_pos_163,
       .group       = false},
      {.path        = k_path_164,
       .description = "Render one entity's template, POST the counterpart to the external system, and\n  record the "
                      "external_links row. Idempotent: if a mirror link already exists\n  for this entity+system pair, the call "
                      "is a no-op and returns op=skipped.\n\n  --from <kind:id>  Source local entity ref (plan:N or task:N)\n  "
                      "--strategy        Override GitHub strategy: parent-issue, projects-v2, tracking-issue",
       .flags       = k_flags_164,
       .positionals = k_pos_164,
       .group       = false},
      {.path        = k_path_165,
       .description = "Propagate a feature (plan + descendants) to an external system.",
       .flags       = k_flags_165,
       .positionals = k_pos_165,
       .group       = false},
      {.path        = k_path_166,
       .description = "Create an entity_links row between two entities.",
       .flags       = k_flags_166,
       .positionals = k_pos_166,
       .group       = false},
      {.path        = k_path_167,
       .description = "List entity_links where the given entity is source or target.",
       .flags       = k_flags_167,
       .positionals = k_pos_167,
       .group       = false},
      {.path        = k_path_168,
       .description = "Delete an entity_links row by its id.",
       .flags       = k_flags_168,
       .positionals = k_pos_168,
       .group       = false},
      {.path        = k_path_169,
       .description = "Show the audit trail for an entity_links row.",
       .flags       = k_flags_169,
       .positionals = k_pos_169,
       .group       = false},
      {.path        = k_path_170,
       .description = "Pull remote state for one or more external links.",
       .flags       = k_flags_170,
       .positionals = k_pos_170,
       .group       = false},
      {.path        = k_path_171,
       .description = "Push local changes for one or more external links.",
       .flags       = k_flags_171,
       .positionals = k_pos_171,
       .group       = false},
      {.path        = k_path_172,
       .description = "Report sync status for links.",
       .flags       = k_flags_172,
       .positionals = {},
       .group       = false},
      {.path        = k_path_173,
       .description = "Settle a sync conflict on a link.",
       .flags       = k_flags_173,
       .positionals = k_pos_173,
       .group       = false},
      {.path = k_path_174, .description = "Check if a task is resumable.", .flags = {}, .positionals = k_pos_174, .group = false},
      {.path        = k_path_175,
       .description = "Create a handoff from an existing snapshot.",
       .flags       = {},
       .positionals = k_pos_175,
       .group       = false},
      {.path = k_path_176, .description = "Validate a pending handoff.", .flags = {}, .positionals = k_pos_176, .group = false},
      {.path        = k_path_177,
       .description = "Mark a handoff as consumed.",
       .flags       = k_flags_177,
       .positionals = k_pos_177,
       .group       = false},
      {.path        = k_path_178,
       .description = "Abandon a non-terminal handoff.",
       .flags       = k_flags_178,
       .positionals = k_pos_178,
       .group       = false},
      {.path = k_path_179, .description = "List handoffs.", .flags = k_flags_179, .positionals = {}, .group = false},
      {.path = k_path_180, .description = "Show a handoff's details.", .flags = {}, .positionals = k_pos_180, .group = false},
      {.path        = k_path_181,
       .description = "Open or reuse a session for the current (vendor, vendor-session-id) tuple.",
       .flags       = k_flags_181,
       .positionals = {},
       .group       = false},
      {.path        = k_path_182,
       .description = "Record explicit git commits into a session.",
       .flags       = k_flags_182,
       .positionals = {},
       .group       = false},
      {.path        = k_path_183,
       .description = "End the active or specified session.",
       .flags       = k_flags_183,
       .positionals = k_pos_183,
       .group       = false},
      {.path        = k_path_184,
       .description = "Append a narrative note to the active session.",
       .flags       = k_flags_184,
       .positionals = k_pos_184,
       .group       = false},
      {.path        = k_path_185,
       .description = "Append a command to the active session.",
       .flags       = k_flags_185,
       .positionals = k_pos_185,
       .group       = false},
      {.path        = k_path_186,
       .description = "Attach a file to the active session.",
       .flags       = k_flags_186,
       .positionals = k_pos_186,
       .group       = false},
      {.path        = k_path_187,
       .description = "Create a context snapshot.",
       .flags       = k_flags_187,
       .positionals = k_pos_187,
       .group       = false},
      {.path = k_path_188,
       .description =
           "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events).",
       .flags       = k_flags_188,
       .positionals = k_pos_188,
       .group       = false},
      {.path        = k_path_189,
       .description = "List commits attributed to sessions and claims.",
       .flags       = k_flags_189,
       .positionals = {},
       .group       = false},
      {.path        = k_path_190,
       .description = "Show the timeline for a session.",
       .flags       = k_flags_190,
       .positionals = k_pos_190,
       .group       = false},
      {.path        = k_path_191,
       .description = "Post the decision body to linked operational-plane targets.",
       .flags       = k_flags_191,
       .positionals = k_pos_191,
       .group       = false},
      {.path        = k_path_192,
       .description = "Check resume-readiness for all in-flight tasks.",
       .flags       = k_flags_192,
       .positionals = {},
       .group       = false},
      {.path        = k_path_193,
       .description = "Find draft plans with zero tasks or only terminal tasks, tasks\n  left doing beyond a threshold, and "
                      "questions left open beyond a\n  threshold. Suggested repair commands are reported but never run.\n\n  "
                      "This reporter always exits 0 when the report is produced, even when\n  findings are present.",
       .flags       = k_flags_193,
       .positionals = {},
       .group       = false},
      {.path = k_path_194,
       .description =
           "Evidence-backed candidate ranking over declared-experiment\n  terminal samples in the exact cohort (plan 950 task "
           "5530). Supply the\n  cohort flags to rank: results report sample and success counts, the raw\n  rate, the 95% Wilson "
           "lower bound, gate-failure rate, and expected excess\n  iterations. Candidates under --min-samples are labelled "
           "insufficient_data\n  and are never ranked or recommended; candidates below --quality-floor are\n  excluded before "
           "any iteration or gate-failure ordering, so a fast-but-wrong\n  candidate cannot outrank a slower correct one.\n\n  "
           "Without cohort flags this falls back to the LEGACY note-convention\n  scorecard below, which remains inspectable but "
           "is not evidence-backed:\n  it predates the routing evidence plane and carries no cohort or\n  independent-quality "
           "guarantee.\n\n  Legacy: read-only aggregation (plan 898/904, tech-spec 520 D8) over the\n  `dispatch_shape` / "
           "`model_choice` note convention in `session_entries`\n  (agents/orchestrator.md step 8a), joined with "
           "`agent_work_claims`\n  (terminal disposition) and `agent_actions` (test-coder expansion\n  outcome). Emits a "
           "per-(work-type, candidate) scorecard and a\n  recommended routing-map change. A pair with no completed-dispatch\n  "
           "history reports insufficient-data rather than a fabricated score.\n  Writes nothing: no routing-map mutation, no "
           "database write. Applying\n  a recommendation is a separate, explicit operator-gated action.",
       .flags       = k_flags_194,
       .positionals = {},
       .group       = false},
      {.path = k_path_195,
       .description =
           "Read-only. Answers \"what tier should this role run at, and is that\n  answer backed by anything?\". Task-bound "
           "roles (coder, test-coder,\n  reviewer, research, janitor) resolve from the task's compiled profile;\n  pre-task "
           "roles (planner, spec-reviewer, ingestor, orchestrator) resolve\n  from a planning packet, which establishes "
           "readiness but classifies no\n  work because no unit of work exists yet.\n\n  When the authoritative packet is absent "
           "or unready the result reports\n  the configured static fallback AND the reason, and never a derived work\n  type — a "
           "tier shown without provenance reads identically to one derived\n  from real evidence.",
       .flags       = k_flags_195,
       .positionals = {},
       .group       = false},
      {.path        = k_path_196,
       .description = "Read-only. Shows each experiment's frozen manifest identity (the\n  cohort it governs, its manifest "
                      "digest, and when an operator approved\n  it) alongside how many terminal samples it has produced and how "
                      "many\n  of those count toward a recommendation. The two counts differ whenever\n  a run was recorded but "
                      "excluded; reporting only the eligible count\n  would understate what actually ran.",
       .flags       = k_flags_196,
       .positionals = {},
       .group       = false},
      {.path        = k_path_197,
       .description = "Read-only. Excluded samples are shown deliberately: they are the\n  audit trail of the evidence boundary. "
                      "Hiding them would make the\n  evidence look thinner than it is and leave no way to check the\n  boundary "
                      "was applied correctly, and showing them without a named\n  reason would look like a bug.",
       .flags       = k_flags_197,
       .positionals = {},
       .group       = false},
      {.path        = k_path_198,
       .description = "Manage opaque operator candidates and host observations.",
       .flags       = {},
       .positionals = {},
       .group       = true},
      {.path        = k_path_199,
       .description = "Decompose workbench spec documents into the task graph.",
       .flags       = k_flags_199,
       .positionals = k_pos_199,
       .group       = false},
      {.path        = k_path_200,
       .description = "Print per-milestone test-spec coverage for an anchor plan.",
       .flags       = k_flags_200,
       .positionals = k_pos_200,
       .group       = false},
      {.path        = k_path_201,
       .description = "Print the resolved configuration.",
       .flags       = k_flags_201,
       .positionals = {},
       .group       = false},
      {.path        = k_path_202,
       .description = "Edit the configuration file in $EDITOR.",
       .flags       = {},
       .positionals = {},
       .group       = false},
      {.path = k_path_203, .description = "Validate configuration file syntax.", .flags = {}, .positionals = {}, .group = false},
      {.path = k_path_204, .description = "Initialize the configuration file.", .flags = {}, .positionals = {}, .group = false},
      {.path = k_path_205, .description = "Show the configuration file path.", .flags = {}, .positionals = {}, .group = false},
      {.path = k_path_206, .description = "List available templates.", .flags = k_flags_206, .positionals = {}, .group = false},
      {.path        = k_path_207,
       .description = "Show a template's raw JSON.",
       .flags       = k_flags_207,
       .positionals = k_pos_207,
       .group       = false},
      {.path        = k_path_208,
       .description = "Render a template against a database entity (dry run; no writes).",
       .flags       = k_flags_208,
       .positionals = k_pos_208,
       .group       = false},
      {.path        = k_path_209,
       .description = "Validate template syntax.",
       .flags       = k_flags_209,
       .positionals = k_pos_209,
       .group       = false},
      {.path        = k_path_210,
       .description = "Extract default templates to disk.",
       .flags       = k_flags_210,
       .positionals = {},
       .group       = false},
      {.path        = k_path_211,
       .description = "Show template resolution paths.",
       .flags       = k_flags_211,
       .positionals = {},
       .group       = false},
      {.path        = k_path_212,
       .description = "List locally-installed skills and agents.",
       .flags       = k_flags_212,
       .positionals = {},
       .group       = false},
      {.path        = k_path_213,
       .description = "Create or reuse symlinks from vendor paths to local source.",
       .flags       = k_flags_213,
       .positionals = k_pos_213,
       .group       = false},
      {.path        = k_path_214,
       .description = "Remove symlinks from vendor paths.",
       .flags       = k_flags_214,
       .positionals = k_pos_214,
       .group       = false},
      {.path        = k_path_215,
       .description = "Import a skill or agent from an external directory.",
       .flags       = k_flags_215,
       .positionals = k_pos_215,
       .group       = false},
      {.path        = k_path_216,
       .description = "Migrate skills/agents to new Planar version.",
       .flags       = k_flags_216,
       .positionals = {},
       .group       = false},
      {.path        = k_path_217,
       .description = "Mint a new run record and print its run_uid.\n\n  --task <id> (repeatable): limit the declared-touch "
                      "snapshot to\n  the given task ids. When omitted, all plan tasks are snapshotted\n  (backward-compatible "
                      "default). Use when the arm only dispatches\n  a known subset of tasks and meta-tasks with no touches "
                      "would\n  otherwise inflate the declared set.",
       .flags       = k_flags_217,
       .positionals = k_pos_217,
       .group       = false},
      {.path        = k_path_218,
       .description = "Append a journal event to a run.",
       .flags       = k_flags_218,
       .positionals = k_pos_218,
       .group       = false},
      {.path        = k_path_219,
       .description = "Record a declared or actual file touch for a run.",
       .flags       = k_flags_219,
       .positionals = k_pos_219,
       .group       = false},
      {.path        = k_path_220,
       .description = "Harvest git diff as actual touches for a run/task.",
       .flags       = k_flags_220,
       .positionals = k_pos_220,
       .group       = false},
      {.path        = k_path_221,
       .description = "Set the terminal status on a run.",
       .flags       = k_flags_221,
       .positionals = k_pos_221,
       .group       = false},
      {.path        = k_path_222,
       .description = "Show a run's full state (header + events + touches).",
       .flags       = k_flags_222,
       .positionals = k_pos_222,
       .group       = false},
      {.path        = k_path_223,
       .description = "Run the extractor over a task's seeds and persist the closure.",
       .flags       = k_flags_223,
       .positionals = k_pos_223,
       .group       = false},
      {.path        = k_path_224,
       .description = "Read back a task's persisted closure rows.",
       .flags       = k_flags_224,
       .positionals = k_pos_224,
       .group       = false},
      {.path        = k_path_225,
       .description = "Mint a new operational run record and print its run_uid as JSON.",
       .flags       = k_flags_225,
       .positionals = {},
       .group       = false},
      {.path        = k_path_226,
       .description = "Append a journal event to a run (seq auto-incremented).",
       .flags       = k_flags_226,
       .positionals = k_pos_226,
       .group       = false},
      {.path        = k_path_227,
       .description = "Set the terminal status on a run.",
       .flags       = k_flags_227,
       .positionals = k_pos_227,
       .group       = false},
      {.path        = k_path_228,
       .description = "Show a run's full state (header + events).",
       .flags       = k_flags_228,
       .positionals = k_pos_228,
       .group       = false},
      {.path        = k_path_229,
       .description = "Recommend closure-minimizing task slices for a plan.",
       .flags       = k_flags_229,
       .positionals = k_pos_229,
       .group       = false},
      {.path        = k_path_230,
       .description = "List shipped and sandbox workflows.",
       .flags       = k_flags_230,
       .positionals = {},
       .group       = false},
      {.path        = k_path_231,
       .description = "Show @meta and source path for a named workflow.",
       .flags       = k_flags_231,
       .positionals = k_pos_231,
       .group       = false},
      {.path        = k_path_232,
       .description = "Resolve <name> across shipped and sandbox workflows, then exec\n  `planar-execute run <path> --phase "
                      "<phase> [--args <json>]\n  [--worktree <dir>] [--sandbox-root <dir>]`.  The workflow's\n  flow.result "
                      "JSON streams to stdout; the exit code is forwarded\n  exactly (non-zero on flow.fail or engine "
                      "error).\n\n  planar-execute resolution order: $PLANAR_EXECUTE_BIN →\n  sibling of argv[0] → PATH.",
       .flags       = k_flags_232,
       .positionals = k_pos_232,
       .group       = false},
      {.path = k_path_233, .description = "Review structured feedback triage.", .flags = {}, .positionals = {}, .group = true},
      {.path        = k_path_234,
       .description = "Append a new step to a plan.",
       .flags       = k_flags_234,
       .positionals = k_pos_234,
       .group       = false},
      {.path        = k_path_235,
       .description = "List steps of a plan.",
       .flags       = k_flags_235,
       .positionals = k_pos_235,
       .group       = false},
      {.path        = k_path_236,
       .description = "Mark a plan step as done.",
       .flags       = k_flags_236,
       .positionals = k_pos_236,
       .group       = false},
      {.path        = k_path_237,
       .description = "Mark a plan step as skipped.",
       .flags       = k_flags_237,
       .positionals = k_pos_237,
       .group       = false},
      {.path        = k_path_238,
       .description = "Associate a plan step with the task that materializes it.",
       .flags       = k_flags_238,
       .positionals = k_pos_238,
       .group       = false},
      {.path        = k_path_239,
       .description = "Declare that a task touches a repo (and, with --path, a specific file).\n\n  Without --path: writes the "
                      "repo-level entity_links 'touches' edge\n  (task -> repo). This is the coarse signal used by `task list "
                      "--touches`.\n\n  With --path <p>: writes a path-level task_touch_paths row (task, repo,\n  path) AND the "
                      "repo-level edge — a path-touch implies the repo-touch, so\n  the repo-level signal stays consistent. <p> "
                      "is a repo-relative file path.\n  The parallelizability rules (`plan recommend-strategy`) read these\n  "
                      "path-level declarations for rules 2/3/4 (disjoint touches, migration\n  touched, singleton file touched). "
                      "Declare path touches per file (repeat\n  the verb), not as a list.",
       .flags       = k_flags_239,
       .positionals = k_pos_239,
       .group       = false},
      {.path = k_path_240,
       .description =
           "Extract path-shaped tokens from a task's title, body, and next_action\n  and resolve them against a repo checkout, "
           "proposing task_touch_paths\n  rows. PREVIEW BY DEFAULT — without --apply nothing is written.\n\n  Each candidate is "
           "classified: 'resolved' (exact file), 'directory'\n  (expanded to its files), 'basename' (every matching path), "
           "'unresolved'\n  (path-shaped but unplaceable) or 'too_broad' (expansion too large).\n\n  Only 'resolved' is written "
           "by default. The wide classifications —\n  directory and basename — are shown with their expansion size and\n  "
           "withheld unless --wide is passed. Measured over 46 tasks in six real\n  plans, including them yielded FEWER "
           "parallel-eligible tasks (13) than\n  resolved-only (14): a wide set intersects peers, and rule 2 drops both\n  sides "
           "of an overlap, so one loose directory mention can remove tasks\n  that were otherwise eligible.\n\n  Proposal still "
           "resolves ambiguity wide (decision 906) — a directory\n  expands, a basename yields every match, nothing unplaceable "
           "is\n  invented. What --wide controls is which proposals are WRITTEN.\n\n  --repo <slug> names the checkout to "
           "resolve against; without it the repo\n  is derived from the current directory (longest matching root_path).",
       .flags       = k_flags_240,
       .positionals = k_pos_240,
       .group       = false},
      {.path        = k_path_241,
       .description = "List the repo- and path-level touches declared on a task.",
       .flags       = k_flags_241,
       .positionals = k_pos_241,
       .group       = false},
      {.path        = k_path_242,
       .description = "Withdraw a touch declaration.\n\n  Without --path: removes the repo-level entity_links 'touches' "
                      "edge.\n\n  With --path <p>: removes ONE path-level task_touch_paths row and leaves\n  the repo edge in "
                      "place. Deliberately not symmetric with `touches add`,\n  where a path-touch implies the repo-touch — "
                      "withdrawing one file should\n  not silently drop a repo claim that may carry other paths.\n\n  Removing "
                      "the repo edge is not a substitute for --path: the parallel\n  eligibility rules read task_touch_paths "
                      "directly, so orphaned path rows\n  keep driving eligibility after their edge is gone.",
       .flags       = k_flags_242,
       .positionals = k_pos_242,
       .group       = false},
      {.path        = k_path_243,
       .description = "Build routing table from workspace membership.",
       .flags       = k_flags_243,
       .positionals = k_pos_243,
       .group       = false},
      {.path        = k_path_244,
       .description = "Display current routing table.",
       .flags       = k_flags_244,
       .positionals = k_pos_244,
       .group       = false},
      {.path        = k_path_245,
       .description = "Register a Jira instance as an external system.",
       .flags       = k_flags_245,
       .positionals = k_pos_245,
       .group       = false},
      {.path        = k_path_246,
       .description = "Register a GitHub Issues repository as an external system.",
       .flags       = k_flags_246,
       .positionals = k_pos_246,
       .group       = false},
      {.path        = k_path_247,
       .description = "List registrations, bindings, and latest observations.",
       .flags       = k_flags_247,
       .positionals = {},
       .group       = false},
      {.path        = k_path_248,
       .description = "Register one exact opaque candidate identifier.",
       .flags       = k_flags_248,
       .positionals = {},
       .group       = false},
      {.path        = k_path_249,
       .description = "Update enabled state and deterministic fallback order.",
       .flags       = k_flags_249,
       .positionals = {},
       .group       = false},
      {.path        = k_path_250,
       .description = "Remove a candidate when no immutable evidence references it.",
       .flags       = k_flags_250,
       .positionals = {},
       .group       = false},
      {.path        = k_path_251,
       .description = "Allow one role and tier for a candidate.",
       .flags       = k_flags_251,
       .positionals = {},
       .group       = false},
      {.path        = k_path_252,
       .description = "Remove one explicit role and tier binding.",
       .flags       = k_flags_252,
       .positionals = {},
       .group       = false},
      {.path        = k_path_253,
       .description = "Append an exact, versioned host capability observation.",
       .flags       = k_flags_253,
       .positionals = {},
       .group       = false},
      {.path        = k_path_254,
       .description = "Report every independent eligibility gate and named exclusion reason.",
       .flags       = k_flags_254,
       .positionals = {},
       .group       = false},
      {.path        = k_path_255,
       .description = "Compare requested and actual spawn identity without aliasing.",
       .flags       = k_flags_255,
       .positionals = {},
       .group       = false},
      {.path        = k_path_256,
       .description = "Export the versioned registry compatibility document.",
       .flags       = k_flags_256,
       .positionals = {},
       .group       = false},
      {.path = k_path_257, .description = "List triaged findings.", .flags = k_flags_257, .positionals = {}, .group = false},
      {.path        = k_path_258,
       .description = "Show a triaged finding.",
       .flags       = k_flags_258,
       .positionals = k_pos_258,
       .group       = false},
      {.path        = k_path_259,
       .description = "Set operator-confirmed triage fields.",
       .flags       = k_flags_259,
       .positionals = k_pos_259,
       .group       = false},
  };
}

auto surface_empty_string_defaults() -> std::span<std::pair<std::string_view, std::string_view> const> {
  // Measured, not guessed: these are the ONLY flags across all three
  // oracle catalogs whose `"default"` is `""` rather than `null` or a
  // typed literal. Verified by diffing this binary's `schema` output
  // against `zig/zig-out/bin/planar schema` byte for byte — before task
  // 6130 those four values were the entire 8-byte difference.
  static constexpr std::pair<std::string_view, std::string_view> k_empty_defaults[] = {
      {"planar workbench edit", "--editor"},
      {"planar workflow run", "--args"},
      {"planar workflow run", "--worktree"},
      {"planar workflow run", "--sandbox-root"},
  };
  return k_empty_defaults;
}

auto surface_summaries() -> std::span<std::pair<std::string_view, std::string_view> const> {
  static constexpr std::pair<std::string_view, std::string_view> k_summaries[] = {
      {"planar", "Planning + agent operations CLI."},
      {"planar init", "Initialize the Planar database and register the current directory as a project."},
      {"planar scope", "Inspect the cwd-derived scope and suggest memberships."},
      {"planar scope show", "Show the cwd-derived scope (and any --scope override)."},
      {"planar scope suggest", "Suggest scope associations based on cwd."},
      {"planar scope use", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope pop", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar scope clear", "Removed in plan 153 M5 — see `planar scope show`."},
      {"planar assoc", "Manage associations (many-to-many scope tags for repos)."},
      {"planar assoc list", "List all known associations."},
      {"planar assoc create", "Create a new association."},
      {"planar assoc add", "Add a repo to an association."},
      {"planar assoc remove", "Remove a repo from an association."},
      {"planar assoc members", "List all project members of an association."},
      {"planar assoc detect", "Propose (or apply) auto-detected associations for the current directory."},
      {"planar plan", "Manage plans and plan steps."},
      {"planar plan create", "Create a new plan."},
      {"planar plan show", "Show a plan's details, steps, and child plans."},
      {"planar plan list", "List plans."},
      {"planar plan update", "Update mutable fields on a plan."},
      {"planar plan edit", "Edit a plan in $EDITOR (editor-first flow)."},
      {"planar plan view", "View a plan's workbench file."},
      {"planar plan diff", "Diff plan against database version."},
      {"planar plan review", "Reviewer entry point for plan diff."},
      {"planar plan link", "Create an entity link from a plan to another entity."},
      {"planar plan next", "Bucketed claim-aware view of next work on a plan (available / claimed / stale / blocked)."},
      {"planar plan recommend-strategy", "Recommend an execution strategy: compute the parallel-eligible subset of a plan's open "
                                         "tasks via the six parallelizability rules."},
      {"planar plan divergence", "Report the declared-vs-derived closure divergence for a plan's open tasks (decision D4)."},
      {"planar plan recompute-status", "Recompute a plan's roll-up status (--plan <id> or --all)."},
      {"planar plan closeout",
       "Delivery-evidence gate: report whether a plan is ready to close and (without --dry-run) mark it done."},
      {"planar plan step", "Manage plan steps."},
      {"planar plan step add", "Append a new step to a plan."},
      {"planar plan step list", "List steps of a plan."},
      {"planar plan step done", "Mark a plan step as done."},
      {"planar plan step skip", "Mark a plan step as skipped."},
      {"planar plan step link", "Associate a plan step with the task that materializes it."},
      {"planar plan descendants", "Emit the anchor plan's full subtree in dependency-topological order. READ-ONLY."},
      {"planar task", "Manage tasks."},
      {"planar task add", "Create a new task."},
      {"planar task show", "Show full task details."},
      {"planar task packet", "Compile the authoritative current routing packet for a task."},
      {"planar task list", "List tasks."},
      {"planar task update", "Update mutable fields on a task."},
      {"planar task edit", "Edit a task in $EDITOR (editor-first flow)."},
      {"planar task view", "View task's workbench file."},
      {"planar task diff", "Diff task against its database-stored version."},
      {"planar task review", "Reviewer entry point for task diff."},
      {"planar task done", "Mark a task as done (single-arg form; Go supports variadic)."},
      {"planar task cancel", "Cancel a task (single-arg form; Go supports variadic)."},
      {"planar task block", "Mark a task as blocked and record the blocking relationship."},
      {"planar task link", "Create an entity link from a task to another entity."},
      {"planar task reopen", "Reopen a done or cancelled task with an audit-trail entry."},
      {"planar task touches", "Manage repo-touches links on a task."},
      {"planar task touches add", "Link a task to a repo via a 'touches' relationship."},
      {"planar task touches infer", "Propose path-level touches from the task's own text (preview by default)."},
      {"planar task touches list", "List the repo- and path-level touches declared on a task."},
      {"planar task touches remove", "Remove a 'touches' link between a task and a repo."},
      {"planar question", "Manage questions."},
      {"planar question add", "Create a new question."},
      {"planar question edit", "Edit a question in $EDITOR (editor-first flow)."},
      {"planar question view", "View question's workbench file."},
      {"planar question diff", "Diff question against database version."},
      {"planar question review", "Reviewer entry point for question diff."},
      {"planar question answer", "Record an answer to a question."},
      {"planar question wontfix", "Mark a question as wontfix."},
      {"planar question list", "List questions."},
      {"planar question show", "Show a question's details."},
      {"planar question link", "Create an entity link from a question to another entity."},
      {"planar scenario", "Manage test scenarios."},
      {"planar scenario add", "Create a new test scenario."},
      {"planar scenario edit", "Edit a scenario in $EDITOR (editor-first flow)."},
      {"planar scenario view", "View scenario's workbench file."},
      {"planar scenario diff", "Diff scenario against database version."},
      {"planar scenario review", "Reviewer entry point for scenario diff."},
      {"planar scenario verify", "Record a test run for a scenario (--outcome pass|fail|error|skipped; defaults to pass)."},
      {"planar scenario retire", "Mark a scenario as retired."},
      {"planar scenario list", "List scenarios."},
      {"planar scenario show", "Show a scenario's details."},
      {"planar scenario link", "Create an entity link from a scenario to another entity."},
      {"planar decision", "Manage decision records."},
      {"planar decision add", "Create a new decision record."},
      {"planar decision show", "Show a decision's details."},
      {"planar decision list", "List decisions."},
      {"planar decision accept", "Accept a proposed decision."},
      {"planar decision supersede", "Mark a decision as superseded by a newer decision."},
      {"planar decision withdraw", "Withdraw a decision."},
      {"planar decision edit", "Edit a decision in $EDITOR (editor-first flow)."},
      {"planar decision view", "View decision's workbench file."},
      {"planar decision diff", "Diff decision against database version."},
      {"planar decision review", "Reviewer entry point for decision diff."},
      {"planar decision link", "Create an entity link from a decision to another entity."},
      {"planar artifact", "Manage artifacts (tech specs, ADRs, design notes, etc.)."},
      {"planar artifact add", "Register a new artifact."},
      {"planar artifact show", "Show an artifact's metadata and body."},
      {"planar artifact list", "List artifacts."},
      {"planar artifact update", "Update mutable fields on an artifact."},
      {"planar artifact edit", "Edit an artifact in $EDITOR (editor-first flow)."},
      {"planar artifact view", "View the artifact's workbench file in $PAGER."},
      {"planar artifact diff", "Show a unified diff between the DB's artifact content and the workbench file."},
      {"planar artifact review", "Reviewer entry point for artifact diff."},
      {"planar artifact link", "Create an entity link from an artifact to another entity."},
      {"planar annotate", "Manage source annotations."},
      {"planar annotate add", "Create a new annotation."},
      {"planar annotate show", "Show an annotation."},
      {"planar annotate list", "List annotations."},
      {"planar annotate update", "Update an annotation."},
      {"planar annotate remove", "Remove an annotation."},
      {"planar annotate tag", "Add or remove a tag on an annotation."},
      {"planar annotate resolve", "Mark an annotation as resolved."},
      {"planar annotate dismiss", "Dismiss an annotation."},
      {"planar annotate archive", "Archive an annotation."},
      {"planar annotate bulk-resolve", "Resolve every active annotation matching the filter."},
      {"planar annotate bulk-dismiss", "Dismiss every active annotation matching the filter."},
      {"planar annotate bulk-archive", "Archive every annotation matching the filter (including non-active rows)."},
      {"planar annotate verify", "Verify annotation anchors against workspace state."},
      {"planar annotate sweep", "Sweep stale annotations (resolved/dismissed older than --since-days)."},
      {"planar promote", "Promote an entity to an association scope."},
      {"planar demote", "Demote an entity back to global scope."},
      {"planar workbench", "Manage workbench sync for plan feature directories."},
      {"planar workbench lint", "Validate workbench Markdown frontmatter without syncing."},
      {"planar workbench pull", "Apply FS→DB changes; report DB→FS drift."},
      {"planar workbench push", "Apply DB→FS changes atomically; report FS→DB drift."},
      {"planar workbench status", "Show drift and conflicts without writing."},
      {"planar workbench resolve", "Settle a sync conflict by choosing FS or DB."},
      {"planar workbench sync", "Atomically apply FS and DB changes via a unified sync."},
      {"planar workbench archive", "Archive a feature's workbench filesystem tree."},
      {"planar workbench restore", "Restore an archived feature's workbench tree."},
      {"planar workbench gc", "Remove FS files whose backing entity is terminal in the DB."},
      {"planar workbench list", "List features with workbench trees."},
      {"planar workbench publish", "Render and push workbench files to external system."},
      {"planar workbench extract-questions", "Parse Open questions from top-level workbench specs (read-only)."},
      {"planar workbench edit", "Edit a feature's workbench files in $EDITOR."},
      {"planar workspace", "Manage workspace state directories and their AGENTS.md surfaces."},
      {"planar workspace init", "Initialize a workspace (org-level association)."},
      {"planar workspace doctor", "Scan and fix workspace registration and state consistency."},
      {"planar workspace routing", "Manage workspace routing table."},
      {"planar workspace routing build", "Build routing table from workspace membership."},
      {"planar workspace routing show", "Display current routing table."},
      {"planar workspace regenerate", "Regenerate AGENTS.md from current state."},
      {"planar ext", "Manage external operational-plane systems (Jira, GitHub Issues, etc.)."},
      {"planar ext register", "Register an external system."},
      {"planar ext register jira", "Register a Jira instance as an external system."},
      {"planar ext register github", "Register a GitHub Issues repository as an external system."},
      {"planar ext list", "List registered external systems."},
      {"planar ext test", "Test connection to an external system."},
      {"planar ext create", "Create an external counterpart for a local entity."},
      {"planar ext propagate-one", "Render + POST + record one entity counterpart; idempotent skip on existing link."},
      {"planar ext propagate", "Propagate a feature (plan + descendants) to an external system."},
      {"planar link", "Link a local entity to an external-system ticket."},
      {"planar unlink", "Remove an external_links row by link id."},
      {"planar links", "List or remove internal entity_links relationships."},
      {"planar links add", "Create an entity_links row between two entities."},
      {"planar links list", "List entity_links where the given entity is source or target."},
      {"planar links remove", "Delete an entity_links row by its id."},
      {"planar links trail", "Show the audit trail for an entity_links row."},
      {"planar sync", "Pull and push data between the local plane and external systems."},
      {"planar sync pull", "Pull remote state for one or more external links."},
      {"planar sync push", "Push local changes for one or more external links."},
      {"planar sync status", "Report sync status for links."},
      {"planar sync resolve", "Settle a sync conflict on a link."},
      {"planar resume", "Produce a structured resume packet for the specified task."},
      {"planar resume validate", "Check if a task is resumable."},
      {"planar handoff", "Capture a context snapshot and create a validated handoff record."},
      {"planar handoff create", "Create a handoff from an existing snapshot."},
      {"planar handoff validate", "Validate a pending handoff."},
      {"planar handoff consume", "Mark a handoff as consumed."},
      {"planar handoff abandon", "Abandon a non-terminal handoff."},
      {"planar handoff list", "List handoffs."},
      {"planar handoff show", "Show a handoff's details."},
      {"planar capture", "Manage explicit session capture."},
      {"planar capture session", "Open or reuse a session for the current (vendor, vendor-session-id) tuple."},
      {"planar capture commits", "Record explicit git commits into a session."},
      {"planar capture end", "End the active or specified session."},
      {"planar capture note", "Append a narrative note to the active session."},
      {"planar capture command", "Append a command to the active session."},
      {"planar capture file", "Attach a file to the active session."},
      {"planar capture snapshot", "Create a context snapshot."},
      {"planar audit", "Cross-plane audit trail commands."},
      {"planar audit trail",
       "Show audit history for an entity (audit_log + entity_links) or an external link (external_links + sync_events)."},
      {"planar audit commits", "List commits attributed to sessions and claims."},
      {"planar audit session", "Show the timeline for a session."},
      {"planar audit publish-decision", "Post the decision body to linked operational-plane targets."},
      {"planar audit handoff-readiness", "Check resume-readiness for all in-flight tasks."},
      {"planar health", "Report database, handoff, and installed-projection health."},
      {"planar health hygiene", "Report stale plan, task, and question lifecycle state without mutating it."},
      {"planar models", "Discover installed provider CLIs and their model catalogs."},
      {"planar models evals",
       "Aggregate completed dispatch outcomes into a per-(work-type, candidate) scorecard and preview-only recommendation."},
      {"planar models resolve", "Resolve a role's routing tier from its authoritative packet, or report the fallback and why."},
      {"planar models experiments", "List declared routing experiments and how much evidence each has produced."},
      {"planar models outcomes", "List recorded terminal outcomes, including excluded ones and why they were excluded."},
      {"planar models registry", "Manage opaque operator candidates and host observations."},
      {"planar models registry list", "List registrations, bindings, and latest observations."},
      {"planar models registry add", "Register one exact opaque candidate identifier."},
      {"planar models registry update", "Update enabled state and deterministic fallback order."},
      {"planar models registry remove", "Remove a candidate when no immutable evidence references it."},
      {"planar models registry bind", "Allow one role and tier for a candidate."},
      {"planar models registry unbind", "Remove one explicit role and tier binding."},
      {"planar models registry observe", "Append an exact, versioned host capability observation."},
      {"planar models registry eligibility", "Report every independent eligibility gate and named exclusion reason."},
      {"planar models registry verify-identity", "Compare requested and actual spawn identity without aliasing."},
      {"planar models registry export", "Export the versioned registry compatibility document."},
      {"planar dashboard", "Operator situational-awareness view of in-flight plans (and, with --agents, live claims)."},
      {"planar spec", "Spec pipeline commands (draft, ingest)."},
      {"planar spec ingest", "Decompose workbench spec documents into the task graph."},
      {"planar test-spec", "Test-spec coverage inspectors."},
      {"planar test-spec status", "Print per-milestone test-spec coverage for an anchor plan."},
      {"planar config", "Manage Planar configuration."},
      {"planar config show", "Print the resolved configuration."},
      {"planar config edit", "Edit the configuration file in $EDITOR."},
      {"planar config validate", "Validate configuration file syntax."},
      {"planar config init", "Initialize the configuration file."},
      {"planar config path", "Show the configuration file path."},
      {"planar templates", "Inspect, validate, and render Planar JSON templates."},
      {"planar templates list", "List available templates."},
      {"planar templates show", "Show a template's raw JSON."},
      {"planar templates render", "Render a template against a database entity (dry run; no writes)."},
      {"planar templates validate", "Validate template syntax."},
      {"planar templates init", "Extract default templates to disk."},
      {"planar templates path", "Show template resolution paths."},
      {"planar tree", "Render plans, tasks, artifacts, decisions, scenarios, and questions as a hierarchical tree."},
      {"planar search", "Full-text search across plans, tasks, questions, scenarios, decisions, and artifacts."},
      {"planar local", "Manage user-local sandbox skills and agents under ~/.planar/local/."},
      {"planar local list", "List locally-installed skills and agents."},
      {"planar local link", "Create or reuse symlinks from vendor paths to local source."},
      {"planar local unlink", "Remove symlinks from vendor paths."},
      {"planar local import", "Import a skill or agent from an external directory."},
      {"planar local migrate", "Migrate skills/agents to new Planar version."},
      {"planar skills", "Retired: rendering and drift detection now live in scriptorium."},
      {"planar import", "Import an existing repo's state into Planar."},
      {"planar synthesize", "Synthesize fresh planning artifacts from a repo's docs + code + git history."},
      {"planar version", "Print the planar version, commit, and zig runtime."},
      {"planar completion", "Generate the autocompletion script for the specified shell."},
      {"planar schema", "Print the full command tree as a JSON catalog (flags, aliases, positionals)."},
      {"planar report", "Emit the diagnostic bundle: invocation and closed claim-failure aggregates plus health metrics."},
      {"planar bench", "Record and query benchmark run data (measurement rig)."},
      {"planar bench start", "Mint a new run record and print its run_uid."},
      {"planar bench event", "Append a journal event to a run."},
      {"planar bench touch", "Record a declared or actual file touch for a run."},
      {"planar bench harvest", "Harvest git diff as actual touches for a run/task."},
      {"planar bench finish", "Set the terminal status on a run."},
      {"planar bench show", "Show a run's full state (header + events + touches)."},
      {"planar closure", "Compute and inspect a task's derived symbol-level closure."},
      {"planar closure compute", "Run the extractor over a task's seeds and persist the closure."},
      {"planar closure show", "Read back a task's persisted closure rows."},
      {"planar run", "Record and query operational run traces."},
      {"planar run start", "Mint a new operational run record and print its run_uid as JSON."},
      {"planar run event", "Append a journal event to a run (seq auto-incremented)."},
      {"planar run finish", "Set the terminal status on a run."},
      {"planar run show", "Show a run's full state (header + events)."},
      {"planar groups", "Recommend task slices that minimize closure replication."},
      {"planar groups recommend", "Recommend closure-minimizing task slices for a plan."},
      {"planar explore", "Launch the interactive cockpit (same as bare `planar` on a TTY)."},
      {"planar workflow", "Discover, inspect, and run Lua workflows for planar-execute."},
      {"planar workflow list", "List shipped and sandbox workflows."},
      {"planar workflow show", "Show @meta and source path for a named workflow."},
      {"planar workflow run", "Resolve a workflow by name and exec it via planar-execute."},
      {"planar feedback", "Manage structured feedback."},
      {"planar feedback triage", "Review structured feedback triage."},
      {"planar feedback triage list", "List triaged findings."},
      {"planar feedback triage show", "Show a triaged finding."},
      {"planar feedback triage set", "Set operator-confirmed triage fields."},
  };
  return k_summaries;
}

auto unported_paths() -> std::span<std::string_view const> {
  static constexpr std::string_view k_unported[] = {
      "artifact add",
      "artifact diff",
      "artifact edit",
      "artifact link",
      "artifact list",
      "artifact review",
      "artifact show",
      "artifact update",
      "artifact view",
      "assoc detect",
      "assoc list",
      "assoc members",
      "assoc remove",
      "audit commits",
      "audit handoff-readiness",
      "audit publish-decision",
      "audit session",
      "audit trail",
      "bench harvest",
      "capture commits",
      "closure compute",
      "config edit",
      "config init",
      "config path",
      "config show",
      "config validate",
      "dashboard",
      "decision accept",
      "decision add",
      "decision diff",
      "decision edit",
      "decision link",
      "decision list",
      "decision review",
      "decision show",
      "decision supersede",
      "decision view",
      "decision withdraw",
      "demote",
      "explore",
      "ext create",
      "ext propagate",
      "ext propagate-one",
      "ext test",
      "feedback triage list",
      "feedback triage set",
      "feedback triage show",
      "health",
      "health hygiene",
      "import",
      "link",
      "links add",
      "links list",
      "links remove",
      "links trail",
      "models resolve",
      "plan closeout",
      "plan descendants",
      "plan diff",
      "plan divergence",
      "plan edit",
      "plan link",
      "plan next",
      "plan recommend-strategy",
      "plan review",
      "plan view",
      "promote",
      "question add",
      "question answer",
      "question diff",
      "question edit",
      "question link",
      "question list",
      "question review",
      "question show",
      "question view",
      "question wontfix",
      "report",
      "scenario add",
      "scenario diff",
      "scenario edit",
      "scenario link",
      "scenario list",
      "scenario retire",
      "scenario review",
      "scenario show",
      "scenario verify",
      "scenario view",
      "scope clear",
      "scope pop",
      "scope show",
      "scope suggest",
      "scope use",
      "search",
      "spec ingest",
      "sync pull",
      "sync push",
      "sync resolve",
      "sync status",
      "synthesize",
      "task diff",
      "task edit",
      "task link",
      "task packet",
      "task review",
      "task touches infer",
      "task view",
      "templates init",
      "templates list",
      "templates path",
      "templates render",
      "templates show",
      "templates validate",
      "test-spec status",
      "tree",
      "workbench edit",
      "workbench extract-questions",
      "workbench publish",
      "workflow run",
      "workspace init",
      "workspace regenerate",
      "workspace routing build",
      "workspace routing show",
  };
  return k_unported;
}

} // namespace planar::cmd
