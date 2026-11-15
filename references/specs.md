# The five specs

Implements PRD-R2 (the five specs), PRD-R5 (diagrams in Mermaid), TS-5 (spec
conventions) and layout entry LAY-1.5.2. This file is the single definition of
what each of the five specs contains and the order they are drafted in. Follow
it whenever you draft or edit a spec. The ID, citation, assumption and question
rules that apply to every spec are in the conventions reference
(${CLAUDE_PLUGIN_ROOT}/references/conventions.md) (TS-5.1 to TS-5.4); the
traceability rules are in the spec review reference,
`references/spec-review.md` (TS-5.5).

## The five specs

The plugin produces five specs as Markdown files in the spec location
(PRD-R2). Each is a separate file and has its own purpose; do not mix them.

| Spec | File | What it contains |
| --- | --- | --- |
| PRD | `prd.md` | What is wanted and why. Requirements (`PRD-R<n>`), each with an ID, and acceptance scenarios written in Gherkin. |
| Tech spec | `tech-spec.md` | How it will be built. Numbered sections (`TS-<n>`, `TS-<n>.<m>`) that say how each requirement is met. |
| Layout | `layout.md` | Which files and directories exist or will exist, and what each holds. One entry per path (`LAY-<n>`). |
| Testing spec | `testing-spec.md` | How each requirement is checked: the checks (`TEST-<id>`), each with its command or question, and a coverage section with one row per requirement. |
| Tasks | `tasks.md` | Ordered units of work, each small enough for one worker (`T-<nnn>`). The entry format is in the task list reference (${CLAUDE_PLUGIN_ROOT}/references/tasks.md). |

The ID, citation, assumption and question rules that apply to every spec are
in the references named at the top of this file; they are not repeated here.

## Order of drafting

Draft in this order, each spec from the ones before it:

1. PRD: the requirements and acceptance scenarios, which say what is wanted.
2. Tech spec: how each requirement is built; it cites the PRD sections it
   implements.
3. Layout: the files and directories the tech spec implies; each entry cites
   the requirement or tech spec section it serves.
4. Testing spec: a check for each requirement, and the coverage section that
   gives every requirement a row.
5. Tasks: the work, cut so that each task has `cites`, `touches` and `checks`
   drawn from the four specs above.

```mermaid
flowchart LR
  PRD --> TS[Tech spec] --> LAY[Layout] --> TEST[Testing spec] --> T[Tasks]
```

## Rules for all five

- IDs, citations, assumptions and open questions follow the conventions
  reference (TS-5.1 to TS-5.4).
- Diagrams are written in Mermaid, inside the Markdown (PRD-R5), as in the
  flowchart above. Do not embed images.
- Traceability is defined and checked as the spec review reference says
  (TS-5.5); draft each spec so that those rules can hold.
