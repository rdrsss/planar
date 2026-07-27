-- routing_candidates: operator registrations of opaque, vendor-namespaced
-- candidate identifiers. Planar assigns no provider meaning to candidate_id.
create table routing_candidates (
  id                    integer primary key autoincrement,
  vendor                text not null check (length(vendor) > 0),
  candidate_id          text not null check (length(candidate_id) > 0),
  enabled               integer not null default 1 check (enabled in (0, 1)),
  fallback_order        integer not null check (fallback_order >= 0),
  registration_version  integer not null default 1 check (registration_version > 0),
  compatibility_source  text not null default 'native' check (
    compatibility_source in ('native', 'legacy_config')
  ),
  created_at             text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at             text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  unique (vendor, candidate_id)
);

create index ix_routing_candidates_fallback
on routing_candidates (vendor, enabled, fallback_order, candidate_id);

-- routing_candidate_bindings: explicit role/tier eligibility for an opaque
-- registration. Absence of a binding never implies eligibility.
create table routing_candidate_bindings (
  candidate_id integer not null references routing_candidates(id) on delete cascade,
  role         text not null check (length(role) > 0),
  tier         text not null check (tier in ('small', 'medium', 'large')),
  created_at   text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  primary key (candidate_id, role, tier)
);

create index ix_routing_candidate_bindings_role_tier
on routing_candidate_bindings (role, tier, candidate_id);

-- routing_host_observations: immutable, versioned observations made by one
-- host about one exact candidate identifier and its spawn verification.
create table routing_host_observations (
  id                  integer primary key autoincrement,
  candidate_id        integer not null references routing_candidates(id) on delete restrict,
  host_id             text not null check (length(host_id) > 0),
  observation_version integer not null check (observation_version > 0),
  availability        text not null check (
    availability in ('available', 'unavailable', 'unknown')
  ),
  spawn_verification  text not null check (
    spawn_verification in ('verified', 'unverified', 'failed', 'mismatch')
  ),
  evidence_ref        text not null check (length(evidence_ref) > 0),
  captured_at         text not null,
  expires_at          text not null,
  created_at          text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (expires_at > captured_at),
  unique (candidate_id, host_id, observation_version)
);

create index ix_routing_host_observations_latest
on routing_host_observations (candidate_id, host_id, observation_version desc);

create index ix_routing_host_observations_expiry
on routing_host_observations (expires_at);

-- routing_task_facts: versioned, provenance-bearing facts materialized from
-- task sources. The typed-value check prevents ambiguous coercion.
create table routing_task_facts (
  id                   integer primary key autoincrement,
  task_id              integer not null references tasks(id) on delete cascade,
  fact_kind            text not null check (length(fact_kind) > 0),
  value_type           text not null check (
    value_type in ('bool', 'integer', 'real', 'text', 'json')
  ),
  value_bool           integer check (value_bool in (0, 1)),
  value_integer        integer,
  value_real           real,
  value_text           text,
  source_entity_kind   text not null check (length(source_entity_kind) > 0),
  source_entity_id     integer not null,
  source_locator       text not null check (length(source_locator) > 0),
  source_digest        text not null check (length(source_digest) > 0),
  materializer_version text not null check (length(materializer_version) > 0),
  created_at           text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  updated_at           text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (value_type = 'bool' and value_bool is not null
      and value_integer is null and value_real is null and value_text is null)
    or (value_type = 'integer' and value_bool is null
      and value_integer is not null and value_real is null and value_text is null)
    or (value_type = 'real' and value_bool is null
      and value_integer is null and value_real is not null and value_text is null)
    or (value_type in ('text', 'json') and value_bool is null
      and value_integer is null and value_real is null and value_text is not null)
  ),
  check (value_type != 'json' or json_valid(value_text)),
  unique (
    task_id, fact_kind, source_entity_kind, source_entity_id, source_locator,
    source_digest, materializer_version
  )
);

create index ix_routing_task_facts_task
on routing_task_facts (task_id, fact_kind, materializer_version);

create index ix_routing_task_facts_source
on routing_task_facts (source_entity_kind, source_entity_id, source_locator);

-- routing_experiments: declarations frozen before outcomes. JSON manifests
-- preserve the exact population, cohort, candidates, and analysis contract.
create table routing_experiments (
  id                        integer primary key autoincrement,
  experiment_key            text not null unique check (length(experiment_key) > 0),
  project_id                integer not null references projects(id) on delete restrict,
  validation_policy_version text not null check (length(validation_policy_version) > 0),
  vendor                    text not null check (length(vendor) > 0),
  role                      text not null check (length(role) > 0),
  tier                      text not null check (tier in ('small', 'medium', 'large')),
  work_type                 text not null check (
    work_type in ('schema', 'engine', 'architectural', 'cli', 'feature', 'mechanical')
  ),
  complexity                text not null check (
    complexity in ('bounded', 'standard', 'high-risk')
  ),
  routing_policy_version    text not null check (length(routing_policy_version) > 0),
  eligible_population_json  text not null check (
    json_valid(eligible_population_json)
    and json_type(eligible_population_json) = 'array'
  ),
  candidate_set_json        text not null check (
    json_valid(candidate_set_json)
    and json_type(candidate_set_json) = 'array'
  ),
  allocation_method         text not null check (
    allocation_method in ('randomized', 'balanced')
  ),
  stopping_rule_json        text not null check (json_valid(stopping_rule_json)),
  analysis_policy_json      text not null check (json_valid(analysis_policy_json)),
  manifest_digest           text not null check (length(manifest_digest) > 0),
  operator_approved_at      text not null,
  status                    text not null default 'declared' check (
    status in ('declared', 'running', 'stopped', 'completed', 'cancelled')
  ),
  created_at                text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

create index ix_routing_experiments_cohort
on routing_experiments (
  project_id, validation_policy_version, vendor, role, tier, work_type, complexity
);

-- routing_dispatch_snapshots: immutable evidence of a confirmed dispatch
-- profile, cohort, packet, policy, requested/actual identity, and disposition.
create table routing_dispatch_snapshots (
  id                        integer primary key autoincrement,
  dispatch_key              text not null unique check (length(dispatch_key) > 0),
  task_id                   integer references tasks(id) on delete set null,
  logical_work_item_id      text not null check (length(logical_work_item_id) > 0),
  project_id                integer not null references projects(id) on delete restrict,
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
  packet_digest             text not null check (length(packet_digest) > 0),
  policy_digest             text not null check (length(policy_digest) > 0),
  capability_digest         text not null check (length(capability_digest) > 0),
  requested_candidate_id    integer not null references routing_candidates(id) on delete restrict,
  actual_vendor             text,
  actual_candidate_id       text,
  assignment_class          text not null check (
    assignment_class in ('fallback', 'default', 'override', 'declared_experiment')
  ),
  experiment_id             integer references routing_experiments(id) on delete restrict,
  operator_decision         text not null check (
    operator_decision in ('confirmed', 'overridden')
  ),
  reviewer_disposition      text not null check (
    reviewer_disposition in ('required', 'approved', 'request_changes', 'bypassed', 'not_reached')
  ),
  terminal_state            text not null default 'pending' check (
    terminal_state in (
      'pending', 'completed', 'quality_failed', 'spawn_failed', 'cancelled',
      'aborted', 'candidate_mismatch', 'missing_evidence'
    )
  ),
  confirmed_at              text not null,
  created_at                text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (actual_vendor is null and actual_candidate_id is null)
    or (actual_vendor is not null and actual_candidate_id is not null)
  ),
  check (
    (assignment_class = 'declared_experiment' and experiment_id is not null)
    or (assignment_class != 'declared_experiment' and experiment_id is null)
  )
);

create index ix_routing_dispatch_snapshots_cohort
on routing_dispatch_snapshots (
  project_id, validation_policy_version, vendor, role, tier, work_type, complexity
);

create index ix_routing_dispatch_snapshots_experiment
on routing_dispatch_snapshots (experiment_id, logical_work_item_id)
where experiment_id is not null;

-- Declared assignments must be members of the frozen experiment manifest and
-- exactly match its cohort. A merely non-null experiment ID is insufficient.
create trigger routing_dispatch_snapshots_experiment_identity
before insert on routing_dispatch_snapshots
when new.assignment_class = 'declared_experiment'
  and not exists (
    select 1
    from routing_experiments as experiment
    where experiment.id = new.experiment_id
      and experiment.project_id = new.project_id
      and experiment.validation_policy_version = new.validation_policy_version
      and experiment.vendor = new.vendor
      and experiment.role = new.role
      and experiment.tier = new.tier
      and experiment.work_type = new.work_type
      and experiment.complexity = new.complexity
      and experiment.routing_policy_version = new.routing_policy_version
      and exists (
        select 1
        from json_each(experiment.eligible_population_json)
        where value = new.logical_work_item_id
      )
      and exists (
        select 1
        from json_each(experiment.candidate_set_json)
        where type = 'integer'
          and value = new.requested_candidate_id
      )
  )
begin
  select raise(abort, 'routing dispatch does not match frozen experiment');
end;

-- routing_dispatch_events: append-only attempts and outcomes. event_id makes
-- replay idempotent; per-dispatch sequence makes out-of-order folding stable.
create table routing_dispatch_events (
  id                  integer primary key autoincrement,
  dispatch_id         integer not null references routing_dispatch_snapshots(id) on delete cascade,
  event_id            text not null unique check (length(event_id) > 0),
  sequence            integer not null check (sequence >= 0),
  event_kind          text not null check (
    event_kind in ('attempt_started', 'attempt_finished', 'outcome', 'supersession')
  ),
  attempt_number      integer check (attempt_number is null or attempt_number > 0),
  terminal_state      text check (
    terminal_state in (
      'completed', 'quality_failed', 'spawn_failed', 'cancelled', 'aborted',
      'candidate_mismatch', 'missing_evidence'
    )
  ),
  supersedes_event_id text references routing_dispatch_events(event_id) on delete restrict,
  payload_json        text not null check (json_valid(payload_json)),
  occurred_at         text not null,
  recorded_at         text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (
    (event_kind = 'outcome' and terminal_state is not null)
    or (event_kind != 'outcome' and terminal_state is null)
  ),
  check (
    (event_kind = 'supersession' and supersedes_event_id is not null)
    or (event_kind != 'supersession' and supersedes_event_id is null)
  ),
  unique (dispatch_id, sequence)
);

create index ix_routing_dispatch_events_fold
on routing_dispatch_events (dispatch_id, sequence, event_id);

-- routing_terminal_samples: one derived terminal sample for a declared
-- experiment identity. The source event is unique and auditable.
create table routing_terminal_samples (
  id                        integer primary key autoincrement,
  experiment_id             integer not null references routing_experiments(id) on delete restrict,
  logical_work_item_id      text not null check (length(logical_work_item_id) > 0),
  role                      text not null check (length(role) > 0),
  initial_packet_digest     text not null check (length(initial_packet_digest) > 0),
  candidate_id              integer not null references routing_candidates(id) on delete restrict,
  project_id                integer not null references projects(id) on delete restrict,
  validation_policy_version text not null check (length(validation_policy_version) > 0),
  routing_policy_version    text not null check (length(routing_policy_version) > 0),
  vendor                    text not null check (length(vendor) > 0),
  tier                      text not null check (tier in ('small', 'medium', 'large')),
  work_type                 text not null check (
    work_type in ('schema', 'engine', 'architectural', 'cli', 'feature', 'mechanical')
  ),
  complexity                text not null check (
    complexity in ('bounded', 'standard', 'high-risk')
  ),
  terminal_event_id         text not null unique references routing_dispatch_events(event_id) on delete restrict,
  terminal_state            text not null check (
    terminal_state in (
      'completed', 'quality_failed', 'spawn_failed', 'cancelled', 'aborted',
      'candidate_mismatch', 'missing_evidence'
    )
  ),
  quality_success           integer not null check (quality_success in (0, 1)),
  cohort_eligible           integer not null check (cohort_eligible in (0, 1)),
  exclusion_reason          text,
  finalized_at              text not null,
  created_at                text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now')),
  check (quality_success = 0 or terminal_state = 'completed'),
  check (
    terminal_state != 'candidate_mismatch'
    or (cohort_eligible = 0 and exclusion_reason is not null)
  ),
  check (
    (cohort_eligible = 1 and exclusion_reason is null)
    or (cohort_eligible = 0 and exclusion_reason is not null)
  ),
  unique (
    experiment_id, logical_work_item_id, role, initial_packet_digest,
    candidate_id, project_id, validation_policy_version, routing_policy_version
  )
);

create index ix_routing_terminal_samples_cohort
on routing_terminal_samples (
  project_id, validation_policy_version, vendor, role, tier, work_type,
  complexity, candidate_id, cohort_eligible
);

-- A derived sample must point to the outcome event for the exact declared
-- experiment assignment and frozen cohort that the sample claims.
create trigger routing_terminal_samples_identity
before insert on routing_terminal_samples
when not exists (
  select 1
  from routing_dispatch_events as event
  join routing_dispatch_snapshots as dispatch
    on dispatch.id = event.dispatch_id
  join routing_candidates as candidate
    on candidate.id = dispatch.requested_candidate_id
  where event.event_id = new.terminal_event_id
    and event.event_kind = 'outcome'
    and event.terminal_state = new.terminal_state
    and dispatch.assignment_class = 'declared_experiment'
    and dispatch.experiment_id = new.experiment_id
    and dispatch.logical_work_item_id = new.logical_work_item_id
    and dispatch.role = new.role
    and dispatch.packet_digest = new.initial_packet_digest
    and dispatch.requested_candidate_id = new.candidate_id
    and dispatch.project_id = new.project_id
    and dispatch.validation_policy_version = new.validation_policy_version
    and dispatch.routing_policy_version = new.routing_policy_version
    and dispatch.vendor = new.vendor
    and dispatch.tier = new.tier
    and dispatch.work_type = new.work_type
    and dispatch.complexity = new.complexity
    and (
      new.cohort_eligible = 0
      or (
        dispatch.actual_vendor = dispatch.vendor
        and dispatch.actual_candidate_id = candidate.candidate_id
      )
    )
)
begin
  select raise(abort, 'routing terminal sample identity mismatch');
end;

-- routing_legacy_outcomes: compatibility-window audit envelope. Missing
-- cohort/quality fields stay readable but can never enter recommendation math.
create table routing_legacy_outcomes (
  id                    integer primary key autoincrement,
  legacy_key            text not null unique check (length(legacy_key) > 0),
  compatibility_version integer not null default 1 check (compatibility_version = 1),
  raw_payload_json      text not null check (json_valid(raw_payload_json)),
  cohort_eligible       integer not null default 0 check (cohort_eligible = 0),
  exclusion_reason      text not null default 'incomplete_legacy_evidence',
  imported_at           text not null default (strftime('%Y-%m-%dT%H:%M:%fZ','now'))
);

-- routing_candidate_compatibility_v1: additive versioned read path. Existing
-- models.* configuration remains untouched and visible alongside registrations.
create view routing_candidate_compatibility_v1 as
select
  1 as compatibility_version,
  'registered' as source,
  vendor,
  candidate_id,
  null as legacy_config_key,
  null as legacy_config_value,
  enabled
from routing_candidates
union all
select
  1 as compatibility_version,
  'legacy_config' as source,
  null as vendor,
  null as candidate_id,
  key as legacy_config_key,
  value as legacy_config_value,
  0 as enabled
from config
where key like 'models.%';

-- Routing audit rows are immutable; later lifecycle facts append events.
create trigger routing_dispatch_snapshots_immutable
before update on routing_dispatch_snapshots
begin
  select raise(abort, 'routing dispatch snapshots are immutable');
end;

create trigger routing_dispatch_snapshots_immutable_delete
before delete on routing_dispatch_snapshots
begin
  select raise(abort, 'routing dispatch snapshots are immutable');
end;

create trigger routing_host_observations_immutable
before update on routing_host_observations
begin
  select raise(abort, 'routing host observations are immutable');
end;

create trigger routing_host_observations_immutable_delete
before delete on routing_host_observations
begin
  select raise(abort, 'routing host observations are immutable');
end;

-- Experiment status may advance, but the pre-outcome manifest is frozen.
create trigger routing_experiments_manifest_immutable
before update of
  experiment_key,
  project_id,
  validation_policy_version,
  vendor,
  role,
  tier,
  work_type,
  complexity,
  routing_policy_version,
  eligible_population_json,
  candidate_set_json,
  allocation_method,
  stopping_rule_json,
  analysis_policy_json,
  manifest_digest,
  operator_approved_at,
  created_at
on routing_experiments
begin
  select raise(abort, 'routing experiment manifests are immutable');
end;

create trigger routing_experiments_immutable_delete
before delete on routing_experiments
begin
  select raise(abort, 'routing experiments are immutable');
end;

create trigger routing_dispatch_events_append_only_update
before update on routing_dispatch_events
begin
  select raise(abort, 'routing dispatch events are append-only');
end;

create trigger routing_dispatch_events_append_only_delete
before delete on routing_dispatch_events
begin
  select raise(abort, 'routing dispatch events are append-only');
end;

create trigger routing_terminal_samples_immutable
before update on routing_terminal_samples
begin
  select raise(abort, 'routing terminal samples are immutable');
end;

create trigger routing_terminal_samples_immutable_delete
before delete on routing_terminal_samples
begin
  select raise(abort, 'routing terminal samples are immutable');
end;

insert into schema_migrations (version, description)
values (30, 'adaptive routing registry, facts, dispatch, experiments, and evidence');
