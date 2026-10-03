-- Entity annotations become thread identities. File annotations deliberately
-- remain in the existing model; no historical text location is inferred.
alter table annotation_operation_receipts add column message_id integer;
alter table annotation_operation_receipts add column message_revision integer;

-- annotation_messages: stable message identities and their current revisions.
create table annotation_messages (
  id integer primary key autoincrement,
  annotation_id integer not null references annotations (id) on delete cascade,
  body text not null,
  revision integer not null default 1 check (revision >= 1),
  vendor text not null default '',
  origin text,
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

create index ix_annotation_messages_thread_order
on annotation_messages (annotation_id, created_at, id);

-- annotation_message_revisions: immutable body snapshots, including the
-- migrated current revision. Historical pre-migration edits are not invented.
create table annotation_message_revisions (
  message_id integer not null references annotation_messages (id) on delete cascade,
  revision integer not null check (revision >= 1),
  body text not null,
  updated_at text not null,
  primary key (message_id, revision)
);

create trigger annotation_message_snapshot_insert after insert on annotation_messages
begin
  insert into annotation_message_revisions (message_id, revision, body, updated_at)
  values (new.id, new.revision, new.body, new.updated_at);
end;

create trigger annotation_message_snapshot_update after update on annotation_messages
begin
  insert into annotation_message_revisions (message_id, revision, body, updated_at)
  values (new.id, new.revision, new.body, new.updated_at);
end;

create trigger annotation_message_snapshot_immutable before update on annotation_message_revisions
begin
  select raise(abort, 'annotation message revisions are immutable');
end;

-- Preserve public identity and revision for every legacy entity note. Using
-- the annotation id as the first message id also makes migration deterministic
-- when databases contain high-valued or sparse ids.
insert into annotation_messages (
  id, annotation_id, body, revision, vendor, origin, created_at, updated_at
)
select
  id as message_id,
  id as thread_id,
  body,
  revision,
  vendor,
  origin,
  created_at,
  updated_at
from annotations
where anchor_kind = 'entity';

-- One optional anchor per entity thread. `page` needs no row; `block` has an
-- ordered block range but no offsets; `range` has UTF-8 boundary offsets and
-- one quote-evidence segment for every covered adjacent block.
-- annotation_contextual_anchors: optional authority-bound thread locations.
create table annotation_contextual_anchors (
  annotation_id integer primary key references annotations (id) on delete cascade,
  schema_version integer not null check (schema_version = 1),
  document_kind text not null check (document_kind in ('plan', 'artifact')),
  document_id integer not null check (document_id > 0),
  document_version integer not null check (document_version = 1),
  content_revision text not null check (length(content_revision) = 64),
  anchor_kind text not null check (anchor_kind in ('block', 'range')),
  start_block_key text not null check (length(start_block_key) between 1 and 512),
  end_block_key text not null check (length(end_block_key) between 1 and 512),
  start_offset integer,
  end_offset integer,
  normalized_quote text not null default '' check (length(normalized_quote) <= 65536),
  prefix_context text not null default '' check (length(prefix_context) <= 1024),
  suffix_context text not null default '' check (length(suffix_context) <= 1024),
  anchor_state text not null default 'attached'
  check (anchor_state in ('attached', 'partially_orphaned', 'orphaned')),
  revision integer not null default 1 check (revision >= 1),
  created_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  updated_at text not null default (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  check ((
    anchor_kind = 'block' and start_offset is null and end_offset is null
    and normalized_quote = ''
  )
  or (
    anchor_kind = 'range' and start_offset is not null and end_offset is not null
    and start_offset >= 0 and end_offset >= 0 and normalized_quote <> ''
  ))
);

-- annotation_anchor_segments: quote evidence for each covered adjacent block.
create table annotation_anchor_segments (
  annotation_id integer not null references annotation_contextual_anchors (annotation_id) on delete cascade,
  ordinal integer not null check (ordinal >= 0),
  block_key text not null check (length(block_key) between 1 and 512),
  quote text not null check (length(quote) between 1 and 16384),
  primary key (annotation_id, ordinal),
  unique (annotation_id, block_key)
);

create index ix_annotation_anchors_document
on annotation_contextual_anchors (document_kind, document_id, start_block_key, end_block_key);
create index ix_annotation_anchor_start
on annotation_contextual_anchors (start_block_key, annotation_id);
create index ix_annotation_anchor_end
on annotation_contextual_anchors (end_block_key, annotation_id);

-- Contextual anchors are entity-only and must name the same authoritative
-- target as their inherited annotation authorization boundary.
create trigger annotation_contextual_anchor_insert before insert on annotation_contextual_anchors
begin
  select case when not exists (
    select 1
    from annotations as a
    where
      a.id = new.annotation_id and a.anchor_kind = 'entity'
      and (
        (new.document_kind = 'plan' and a.target_kind = 'plan' and a.target_id = new.document_id)
        or (
          new.document_kind = 'artifact' and a.target_kind = 'plan'
          and exists (
            select 1 from entity_links as x
            where
              x.from_kind = 'plan' and x.from_id = a.target_id
              and x.to_kind = 'artifact' and x.to_id = new.document_id
          )
        )
      )
  ) then raise(abort, 'contextual anchor target is outside annotation authorization') end;
end;

create trigger annotation_contextual_anchor_update before update on annotation_contextual_anchors
begin
  select case when not exists (
    select 1
    from annotations as a
    where
      a.id = new.annotation_id and a.anchor_kind = 'entity'
      and (
        (new.document_kind = 'plan' and a.target_kind = 'plan' and a.target_id = new.document_id)
        or (
          new.document_kind = 'artifact' and a.target_kind = 'plan'
          and exists (
            select 1 from entity_links as x
            where
              x.from_kind = 'plan' and x.from_id = a.target_id
              and x.to_kind = 'artifact' and x.to_id = new.document_id
          )
        )
      )
  ) then raise(abort, 'contextual anchor target is outside annotation authorization') end;
end;

-- Message rows cannot accidentally turn file notes into threads.
create trigger annotation_message_insert before insert on annotation_messages
when
  not exists (
    select 1 from annotations as a
    where a.id = new.annotation_id and a.anchor_kind = 'entity'
  )
begin
  select raise(abort, 'annotation messages require an entity thread');
end;

-- Existing creation/update commands remain compatibility writers for the
-- first message, including after replies. Message edits synchronize the
-- legacy body separately; equal bodies avoid a second message revision.
create trigger annotation_entity_thread_insert after insert on annotations
when new.anchor_kind = 'entity'
begin
  insert into annotation_messages (
    annotation_id, body, revision, vendor, origin, created_at, updated_at
  ) values (
    new.id, new.body, new.revision, new.vendor, new.origin, new.created_at, new.updated_at
  );
end;

create trigger annotation_legacy_message_update after update of body on annotations
when
  new.anchor_kind = 'entity'
  and new.body <> (
    select m.body from annotation_messages as m
    where m.annotation_id = new.id
    order by m.created_at, m.id limit 1
  )
begin
  update annotation_messages
  set body = new.body, revision = revision + 1, updated_at = new.updated_at
  where id = (
    select m.id from annotation_messages as m
    where m.annotation_id = new.id
    order by m.created_at, m.id limit 1
  );
end;

-- Segments append in exact ordinal order. This prevents reordered,
-- discontinuous representations at the storage boundary; document adjacency
-- itself is checked by the projection-aware command before this insert.
create trigger annotation_anchor_segment_insert before insert on annotation_anchor_segments
begin
  select case when not exists (
    select 1 from annotation_contextual_anchors
    where annotation_contextual_anchors.annotation_id = new.annotation_id and annotation_contextual_anchors.anchor_kind = 'range'
  ) then raise(abort, 'anchor segments require a range anchor') end;
  select case when new.ordinal <> (
    select count(*) from annotation_anchor_segments
    where annotation_anchor_segments.annotation_id = new.annotation_id
  ) then raise(abort, 'anchor segments must be appended in ordinal order') end;
  select case when new.ordinal = 0 and new.block_key <> (
    select annotation_contextual_anchors.start_block_key from annotation_contextual_anchors
    where annotation_contextual_anchors.annotation_id = new.annotation_id
  ) then raise(abort, 'first anchor segment must match start block') end;
end;

create trigger annotation_message_revision before update on annotation_messages
when
  new.id <> old.id
  or new.annotation_id <> old.annotation_id
  or new.created_at <> old.created_at
  or new.revision <> old.revision + 1
begin
  select raise(abort, 'annotation message edit requires the next revision');
end;

create trigger annotation_anchor_revision before update on annotation_contextual_anchors
when
  new.annotation_id <> old.annotation_id
  or new.created_at <> old.created_at
  or new.revision <> old.revision + 1
begin
  select raise(abort, 'annotation re-anchor requires the next revision');
end;

create trigger annotation_anchor_reanchor_audit after update on annotation_contextual_anchors
begin
  insert into audit_log (verb, entity_kind, entity_id, summary)
  values ('update', 'annotation', new.annotation_id, 'explicit contextual re-anchor');
end;

insert into schema_migrations (version, description)
values (41, 'contextual annotation threads, revisioned messages, and adjacent range anchors');
