# Spec conventions

Implements PRD-R3 (stable section IDs), PRD-R4 (evidence), PRD-R6 (open
questions) and TS-5 (spec conventions, except traceability). This file is the
single definition of ID prefixes, citation forms, assumptions, question
sections and the ID index. Follow it whenever you write, edit or check a spec.
The traceability rules (every requirement is built and tested, every task
cites something) are defined in the spec review reference, not here.

## IDs

Give every section that another document may refer to an ID. Put the ID at
the start of the heading or bullet that introduces the section, for example
`## TS-5 Spec conventions` or `- **PRD-R3 Stable section IDs.** ...`.

Use these prefixes:

| Prefix | Identifies |
| --- | --- |
| `PRD-` | a product requirement or section (`PRD-R3`) |
| `TS-` | a tech spec section (`TS-5.2`) |
| `LAY-` | a layout section (`LAY-1.5.1`) |
| `TEST-` | a testing spec section or check (`TEST-C7`) |
| `T-` | a task, zero-padded to three digits (`T-001`, `T-042`) |
| `G-` | a gap |
| `D-` | a decider decision |

Rules:

- An ID is unique within the project, not just within its file.
- An ID never changes when its document is edited. Reword the text, move the
  section, renumber nothing.
- Never reuse an ID, including one whose section was removed.
- To remove a section, keep its ID in the ID index and mark it `withdrawn`.
  In the spec, leave a one-line stub in place of the section, with the ID, the
  word "Withdrawn" and, if another section replaces it, that section's ID, for
  example `TS-8.9 Withdrawn; see TS-19.1`. Other documents that cite the ID
  then still resolve.
- Add new sections with new IDs; do not insert between and renumber.

## ID index (`ids.md`)

`ids.md` lists every ID ever issued, one per line, as three fields
separated by ` | `: the ID, the file that holds it, and its status, which is
`live` or `withdrawn`. The file starts with the heading `# IDs issued`,
followed by a second line giving the date and the check report that wrote the
index. When you update the index, keep both header lines and match the
existing line form exactly. The index is the baseline for detecting an ID that
changed or vanished: an ID in the index that is missing from its file and not
`withdrawn` is a failure.

```
# IDs issued

PRD-R3 | prd.md | live
T-004 | tasks.md | withdrawn
```

(In the real file the second line is the date and report line described
above, not blank; copy its form from the existing `ids.md`.)

When you add a section, add its line to `ids.md` as `live`. When you remove
one, change its line to `withdrawn`; never delete a line.

## Citations

A claim of fact in a spec carries a citation. A citation is exactly one of:

- a bare ID: `PRD-R4`, `TS-5.2`
- a file with a line or a section: `src/main.cpp:120`, `README.md, "Install"`
- a URL: `https://example.com/page`
- an operator decision with its date: `operator decision, 2026-11-14`. The
  date must match a dated entry in the project's decisions list in
  `context.md`; look the decision up there before citing it.

Anything stated as fact that has no such source is marked as an assumption:

```
*Assumption:* the plugin loads references on demand.
```

Write `*Assumption:*` immediately before the claim, in italics, as shown. Do
not present an unsourced claim as settled; do not leave it unmarked. When a
source later confirms an assumption, replace the marker with the citation.

## Open questions

Every spec ends with two sections, in this order: "Open questions" and
"Answered questions". Each question has an ID (unique in the project, formed
with the spec's own prefix, for example `PRD-Q1`) and one line stating what
must be decided.

The operator answers questions. In an autonomous run the decider may answer
them in the operator's place. No other agent answers a question: a worker,
reviewer or planner that needs an answer leaves the question open and reports
it. When a question is answered, move the question from "Open questions" to "Answered questions" and
record, on the same entry:

- the answer (and, if the decider answered, that it did, with its `D-` ID);
- the date of the answer;
- the ID of the section that implements the answer.

```
- **PRD-Q1** Should specs be replicated into the repository?
  Answered 2026-11-14: yes, on request only. Implemented in TS-11.4.
```

If a spec has none, keep both sections and write "None." in each.
