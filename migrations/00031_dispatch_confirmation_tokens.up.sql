-- routing_dispatch_previews: the binding between "here is what I am about to
-- dispatch" and "do it". A preview freezes every value the operator was shown
-- — packet and profile digests, policy versions, capability snapshot, cohort,
-- candidate, claim target — and hands back a single-use, expiry-bound token.
--
-- Confirm revalidates each bound value against current state before writing a
-- dispatch snapshot. That is the whole point of storing them: without the
-- frozen copy there is nothing to compare against, and "confirm" would mean
-- "re-derive and hope nothing moved" — which silently dispatches against state
-- the operator never saw.
--
-- consumed_at makes the token single-use. Reuse is not idempotent replay: the
-- second confirm may land in a world where the packet has changed, so it is
-- rejected as stale rather than quietly re-confirmed.
create table routing_dispatch_previews (
  id                        integer primary key autoincrement,
  preview_token             text not null unique check (length(preview_token) > 0),
  task_id                   integer references tasks(id) on delete cascade,
  logical_work_item_id      text not null check (length(logical_work_item_id) > 0),
  project_id                integer not null references projects(id) on delete restrict,

  -- Bound cohort. Confirm rejects any drift here: a dispatch that changed
  -- cohort between preview and confirm belongs to a different experiment.
  validation_policy_version text not null check (length(validation_policy_version) > 0),
  routing_policy_version    text not null check (length(routing_policy_version) > 0),
  profile_rule_version      text not null check (length(profile_rule_version) > 0),
  vendor                    text not null check (length(vendor) > 0),
  role                      text not null check (length(role) > 0),
  tier                      text not null check (tier in ('small', 'medium', 'large')),
  work_type                 text not null check (
    work_type in ('schema', 'engine', 'architectural', 'cli', 'feature', 'mechanical')
  ),
  complexity                text not null check (
    complexity in ('bounded', 'standard', 'high-risk')
  ),

  -- Bound digests. Any change to the task, the profile, or the host capability
  -- snapshot invalidates the preview.
  packet_digest             text not null check (length(packet_digest) > 0),
  profile_digest            text not null check (length(profile_digest) > 0),
  policy_digest             text not null check (length(policy_digest) > 0),
  capability_digest         text not null check (length(capability_digest) > 0),

  -- Bound target. requested_candidate_id is what routing chose; host_id names
  -- the machine whose capability snapshot was consulted, because eligibility
  -- is host-scoped and a different host is a different answer.
  requested_candidate_id    integer not null references routing_candidates(id) on delete restrict,
  host_id                   text not null check (length(host_id) > 0),
  delegated_candidate_id    integer references routing_candidates(id) on delete restrict,
  assignment_class          text not null check (
    assignment_class in ('fallback', 'default', 'override', 'declared_experiment')
  ),
  experiment_id             integer references routing_experiments(id) on delete restrict,

  -- Bound claim target. A claim that moved to another agent between preview
  -- and confirm means the work is no longer ours to dispatch.
  claim_token               text,
  claim_status_at_preview   text,

  -- Named exclusions and evidence state shown to the operator, retained so the
  -- audit trail records what they were told, not just what was decided.
  exclusions_json           text not null default '[]' check (
    json_valid(exclusions_json) and json_type(exclusions_json) = 'array'
  ),
  evidence_state            text not null check (
    evidence_state in ('evidential', 'observational')
  ),

  created_at                text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  expires_at                text not null check (length(expires_at) > 0),
  consumed_at               text,
  consumed_dispatch_id      integer references routing_dispatch_snapshots(id) on delete set null,

  -- A consumed preview must name the dispatch it produced. The pair is what
  -- makes "which preview authorized this dispatch?" answerable after the fact.
  check (
    (consumed_at is null and consumed_dispatch_id is null)
    or (consumed_at is not null and consumed_dispatch_id is not null)
  ),
  -- Only a declared experiment may name an experiment, mirroring the
  -- constraint on routing_dispatch_snapshots so a preview cannot promise a
  -- dispatch the snapshot table would reject.
  check (
    (assignment_class = 'declared_experiment' and experiment_id is not null)
    or (assignment_class != 'declared_experiment' and experiment_id is null)
  )
);

create index ix_routing_dispatch_previews_task
on routing_dispatch_previews (task_id)
where task_id is not null;

create index ix_routing_dispatch_previews_open
on routing_dispatch_previews (expires_at)
where consumed_at is null;

-- A consumed preview is immutable. Rewriting which dispatch a token authorized
-- would destroy the audit link the consumed pair exists to provide.
create trigger trg_routing_dispatch_previews_single_use
before update on routing_dispatch_previews
for each row
when old.consumed_at is not null
begin
  select raise(abort, 'confirmation token already consumed');
end;

insert into schema_migrations (version, description)
values (31, 'dispatch confirmation tokens');
