# Swarm orchestration

This is one orchestration that works: a single orchestrator session runs a plan
by dispatching small, parallel worker sessions, one worker per task, wave by
wave. Planar holds the state and enforces the claim and review rules. Every
other choice below (how many workers, which models, when tests run, who merges)
is this example's choice and can be changed. The prompt in the last section is
meant to be pasted to the orchestrator and edited.

## Adapt these

| Knob | This example's value | Change it when |
|------|----------------------|----------------|
| Worker model | A small, fast model for code tasks | A task needs more judgment than the small model gives. |
| Writer model | A mid-size model for docs and prose tasks | Prose quality matters more than cost. |
| Reviewer model | A stronger model, a different session from every worker | You review the work yourself, or your policy does not set `review_required`. |
| Wave size | At most 4 tasks at a time | Your machine, queue or review capacity is larger or smaller. |
| Test cadence | Targeted tests per task, the full suite once per milestone | You want a full run per task, or only at the end. |
| Reviewer cadence | One review pass per milestone | You want one per task. |
| Merge policy | Workers commit in their own worktrees; the orchestrator merges into the plan branch; the operator merges the plan branch | You want a pull request per task, or no branches. |
| Build and test runner | The host queue, `planar-agent queue run` | Your policy has `use_queue = false` and you run commands directly. |

Planar requires none of these. Two of them interact with policy: when the
scope's policy sets `review_required`, `complete` refuses until a different
session has recorded an approving review on the claim; when it sets
`task_gate`, `complete` also needs a passing queued run of the gate command
(see [the queue reference](../skills/planar/references/queue.md#gate-evidence)).

## Roles

- **Orchestrator.** Reads state, prepares tasks, takes claims, writes worker
  briefs, checks integration, runs the terminal verb. It writes no production
  code.
- **Workers.** One session per task, each in its own git worktree. A worker
  heartbeats and returns a short result. It never ends a claim.
- **Reviewer.** A session that is neither the orchestrator nor a worker. It
  reads the diff and records the verdict with `planar-agent review`.

## The loop

1. **Read the brief.** `planar-agent brief <plan-id> --json` shows the plan's
   state, live and stale claims, the parallel wave, readiness per task and the
   effective policy. Run it again after every wave; the next wave comes from it.
2. **Take a coordination claim.**
   `planar-agent claim --entity plan:<plan-id> --role orchestrator --vendor <vendor> --model <model> --ttl 8h --json`.
   Keep its `action_id`; every worker claim passes it as `--parent-action`, so
   `planar-watch tree` shows the workers under the orchestrator.
3. **Prepare each task** from the parent checkout (planning writes refuse inside
   a worktree). A task is ready when `planar task packet <task-id> --json` shows
   `"ready": true`. Prepare what your plan needs:
   - `planar task touches add <task-id> <repo-slug> --path <file>` for each file
     the task changes; `plan recommend-strategy` reads touches to decide which
     tasks can run together.
   - `planar task link <task-id> decision:<id> --relationship cites` for each
     decision that governs it.
   - `planar task link <task-id> task:<other-id> --relationship depends-on` for
     ordering.
   - Answer or withdraw whatever the packet's `reasons` name
     (`planar question answer`, `planar decision accept`). If a reason cannot be
     cleared, stop and report; do not claim the task with `--allow-unready`.
4. **Choose a wave.** `planar plan recommend-strategy <milestone-plan> --json`
   lists the tasks that can run in parallel; take at most the wave size.
   Serialized tasks run one per wave, in dependency order.
5. **Claim on each worker's behalf.** For each task in the wave, create its
   worktree and branch, then:

   ```sh
   planar-agent claim --entity task:<task-id> --role coder --vendor <vendor> \
     --model <worker-model> --vendor-session <vendor>:<worker-label> \
     --parent-action <action-id> --worktree <worker-worktree> --ttl 4h --json
   ```

   Keep the `claim_token`. The `--vendor-session` value is what a review later
   compares against the reviewer's session, so give each worker its own.
6. **Dispatch** every worker of the wave in the background, each with: the task
   id and title, its claim token, its worktree path and branch, the excerpt of
   the spec that governs it (read a cited section with
   `planar task packet <task-id> --expand 'artifact:<id>#<heading>'`), and its
   declared files. The brief tells the worker to heartbeat, commit with
   `git commit -s` on explicit paths, return the commit sha and test result, and
   never run `planar-agent complete`, `fail`, `release` or `block`, or any
   `planar task` status verb.
7. **Integrate as each worker returns.** Heartbeat every live claim, re-run the
   task's targeted tests, merge the worker's branch into the plan branch, and
   confirm the commit arrived:

   ```sh
   git -C <plan-worktree> merge-base --is-ancestor <worker-sha> HEAD && echo integrated
   ```

   A merge of a misspelt branch name can print "Already up to date" and exit 0,
   so the ancestry check is the evidence, not the merge. Then remove the
   worker's worktree.
8. **Review once per milestone.** When the milestone's tasks are integrated, a
   reviewer session reads the milestone diff against the spec and records one
   verdict on each claim in the milestone:

   ```sh
   planar-agent review --claim <worker-token> --verdict approve \
     --summary "<one line>" --vendor-session <vendor>:<reviewer-label>
   ```

   The reviewer's session must differ from the reviewed claim's session, or the
   verb refuses with `ReviewerNotIndependent`. On `request-changes` the
   orchestrator sends the same worker, or a new one, back with the findings and
   reviews again; the claim stays active meanwhile.
9. **End each claim with exactly one terminal verb**, run by the orchestrator,
   only after the work is integrated by this example's definition (the commit is
   an ancestor of the plan branch head) and, where policy requires it, after the
   approving review:

   ```sh
   planar-agent complete --claim <worker-token> --summary "<one line + sha>"
   ```

   If a worker fails twice, use `planar-agent fail --claim <token> --reason
   "<text>"`; if another task blocks it, `planar-agent block --claim <token>
   --blocker <task-id>`. Do not split the ending into `planar task done` plus a
   release; a crash between the two strands the claim.
10. **Repeat** from step 1 for the next wave, then the next milestone. When the
    last milestone is done, release the coordination claim with
    `planar-agent release --claim <orchestrator-token>`.
11. **Check the trail.** `planar health conformance <plan-id>` grades the
    recorded claims, models, parent actions, gate runs and reviews against the
    scope's policy and changes nothing. Fix what it reports before calling the
    plan finished.

## Builds and tests through the host queue

This is an operator choice (`use_queue` in `config.toml`; see
[the queue reference](../skills/planar/references/queue.md)). When it is on,
workers and the orchestrator send expensive commands through the queue and pass
the claim so the lease is renewed while they wait:

```sh
planar-agent queue run --claim <token> --vendor <vendor> --role coder -- <test command>
planar-agent queue run --detach --vendor <vendor> --role orchestrator -- <full suite command>
planar-agent queue wait <seq> --timeout 3h --json
```

This example runs targeted tests per task, a full suite once per milestone, and
reads a failing log in full before acting. If `queue run` exits 125 the queue
refused: report it and do not run the command directly. When the policy has a
`task_gate`, the gate command must be submitted exactly as the policy gives it,
with `--claim <token>`, from the claim's worktree.

## Resuming after a restart

Rebuild state from Planar, not from memory: `planar-agent brief <plan-id>`,
`planar-watch ps --stale` for live and stale claims, `git log` on the plan
branch and `git worktree list`. Heartbeat the claims that are still yours, then
continue from step 1. A claim that lapsed while its work was done is re-taken
with `planar-agent claim --entity task:<id> --no-transition --json` and then
ended with the one terminal verb.

## The orchestrator prompt

Paste this, fill in the angle-bracket values, and delete the lines that do not
fit your plan.

```text
You are the orchestrator for Planar plan <plan-id>. You coordinate and verify;
you never write production code. Use the `planar`, `planar-agent` and
`planar-watch` CLIs for all state, never the database.

Checkout: <repo-checkout>. Plan branch: <plan-branch>, in <plan-worktree>.
Worker model: <worker-model>. Writer model: <writer-model>.
Wave size: <n>. Merge policy: <orchestrator merges into the plan branch; the
operator merges the plan branch>.

Start with `planar-agent brief <plan-id> --json`. Prepare every todo task so
`planar task packet <id> --json` shows ready: true. Take the next wave from
`planar plan recommend-strategy`. For each task, make a worktree and branch off
<plan-branch>, claim it for a worker with --parent-action, --vendor-session and
--worktree, and brief the worker with the claim token, the task, its governing
spec excerpt and its files. Workers heartbeat and return; they never run a
terminal verb. After a worker returns, re-run its targeted tests through
`planar-agent queue run`, merge its branch and confirm
`git merge-base --is-ancestor <sha> HEAD`.

When a milestone is integrated, have a separate reviewer session record
`planar-agent review` on each claim. Then run exactly one terminal verb per
claim. When the plan is done, release your coordination claim and run
`planar health conformance <plan-id>`. Report once per milestone: tasks done,
test counts, review verdicts, branch head sha.

Stop and ask me if a task cannot be made ready, a reviewer aborts, the queue
refuses with exit 125, or the spec contradicts the code.
```
