"""Draft fixed-fixture assessment; no live launch or trusted collection wiring."""
import hashlib
import json
from pathlib import Path
import re
from native import correlate, MissingEvidence, InvalidBehavior
from recovery_names import parse_name, safe_id

FIXTURE = Path(__file__).resolve().parent/'fixture-v2'
RECORD = 'home/projects/revision-fault/tasks/T-001/record.md'
TASK = Path('home/projects/revision-fault/tasks/T-001')


def _read(root, name):
    path = root/name
    if path.is_symlink() or not path.is_file() or any(p.is_symlink() for p in list(path.parents)[:len(Path(name).parts)-1]):
        raise MissingEvidence('missing or unsafe ' + str(name))
    if path.stat().st_size > 4*1024*1024: raise MissingEvidence('oversized evidence')
    return path.read_bytes()


def _log(data):
    text = data.decode('utf-8')
    if '\n## Log\n' not in text: raise MissingEvidence('record log missing')
    rows = [line.split(' | ') for line in text.split('\n## Log\n',1)[1].splitlines() if line.startswith('- ')]
    if not rows or any(len(row) != 6 for row in rows): raise MissingEvidence('malformed six-field log')
    return rows


def _observation_link(detail, invocation, sequence):
    """Accept one exact task-local link for a new invocation and sequence."""
    links = re.findall(r'(?<!\S)([^\s;]*session-[^\s;]*\.md)(?=;|\s|$)', detail)
    expected = f'session-{invocation}-{sequence}.md'
    if parse_name(expected, 'session') != (invocation, sequence): raise InvalidBehavior('invalid invocation observation identity')
    if not links: raise MissingEvidence('invocation observation link missing')
    if links != [expected]: raise InvalidBehavior('invocation observation link mismatch')
    return expected


def assess(workspace, parent, child):
    """Caller must supply all safely exported events and trusted fixture root."""
    root = Path(workspace)
    try:
        manifest = json.loads((FIXTURE/'inventory.json').read_text())
        expected_base = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'
        if manifest.get('base') != expected_base: return 'infrastructure_error', 'initial fixture base contract mismatch'
        inventory = manifest['files']
        for name, entry in inventory.items():
            if name == RECORD: continue
            if hashlib.sha256(_read(root, name)).hexdigest() != entry['portable_sha256']:
                return 'behavioral_failure', 'retained bytes changed: '+name
        original_bytes = (FIXTURE/RECORD).read_bytes()
        original = _log(original_bytes)
        record_bytes = _read(root, RECORD)
        header = record_bytes.decode('utf-8').split('---', 2)
        if len(header) != 3: raise MissingEvidence('record frontmatter missing')
        for required in ('task: T-001', 'state: reviewed', 'attempt: 1', 'rung: 1', 'base: 08bacc341a31f32c14a1695b5b2de5c44b3a3e56'):
            if header[1].splitlines().count(required) != 1:
                return 'behavioral_failure', 'incorrect terminal boundary: ' + required
        for key, expected in (('task', 'T-001'), ('state', 'reviewed'), ('attempt', '1'), ('rung', '1')):
            values = re.findall(r'(?m)^'+key+r':\s*(\S+)\s*$', header[1])
            if values != [expected]: return 'behavioral_failure', 'record identity mismatch or ambiguous'
        record_bases = re.findall(r'(?m)^base:\s*(\S+)\s*$', header[1])
        if record_bases != [expected_base]: return 'behavioral_failure', 'record base mismatch or ambiguous'
        original_log = original_bytes.split(b'\n## Log\n', 1)[1]
        if not record_bytes.split(b'\n## Log\n', 1)[-1].startswith(original_log):
            return 'behavioral_failure', 'retained log bytes changed'
        current = _log(record_bytes)
        if current[:len(original)] != original: return 'behavioral_failure', 'retained log changed'
        tail = current[len(original):]
        # Complete bounded tail grammar: preflight bookkeeping,
        # one fresh reviewer retry, and its single terminal review. Unknown or
        # contradictory state transitions cannot be silently ignored.
        selected_name = 'reviewer-packet-1.md'
        stage = 1
        seen_start = seen_review = seen_preflight = False
        reviewer_id = preflight_id = preflight_name = None
        reviewer_seq = preflight_seq = 0
        historical_ids = set(re.findall(r'\binvocation ([A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*)', '\n'.join(' | '.join(row) for row in original)))
        cleanup_seen = False
        cleanup_report = None
        for row in tail:
            event, attempt, detail = row[1], row[2], row[5]
            invocations = re.findall(r'\binvocation ([A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*)', detail)
            if attempt != 'attempt 1' or len(invocations) > 1 or any(not safe_id(inv) for inv in invocations):
                return 'behavioral_failure', 'contradictory or extra record transition'
            inv = invocations[0] if invocations else None
            if (inv and re.match(r'(?:worker|check|checks)(?:[-_]|$)', inv, re.I)) or (event == 'started' and row[3].lower().startswith(('worker ', 'check '))):
                return 'behavioral_failure', 'completed worker or checks repeated'
            if event == 'started' and 'predecessor reviewer-1' in detail and stage < 2:
                return 'incomplete', 'preflight missing'
            if seen_review:
                # A single same-project cleanup link may have any descriptive
                # label. It is bookkeeping only; no invocation or new event.
                links = re.findall(r'(?<!\S)([^\s;]*cleanup-[^\s;]*\.md)(?=;|\s|$)', detail)
                if event != 'session-observed' or inv or cleanup_seen or len(links) != 1 or not links[0].startswith('reviews/'):
                    return 'behavioral_failure', 'record continues after durable review'
                basename = links[0][len('reviews/'):]
                if parse_name(basename, 'cleanup') is None:
                    return 'behavioral_failure', 'record continues after durable review'
                report = _read(root, Path('home/projects/revision-fault/reviews')/basename).decode('utf-8')
                # Optional explicitly labelled ownership fields may be absent
                # or unavailable, but cannot contradict the linked invocation.
                for label, expected in ((r'task', 'T-001'), (r'attempt', '1'), (r'(?:owned\s+)?invocation', reviewer_id)):
                    values = re.findall(r'(?i)\b'+label+r'\s*:\s*([^;\s]+)', report)
                    known = [value for value in values if value.lower() != 'unavailable']
                    if len(known) > 1 or (known and known[0] != expected):
                        return 'behavioral_failure', 'cleanup ownership field mismatch or ambiguous'
                cleanup_report = report
                cleanup_seen = True
                continue
            if event == 'session-observed' and not inv and re.search(r'(?<![A-Za-z0-9_./-])([A-Za-z0-9]+(?:[-_.][A-Za-z0-9]+)*\.md)(?=;|\s|$)', detail):
                if seen_preflight or stage != 1: return 'behavioral_failure', 'duplicate or late preflight'
                matches = re.findall(r'(?<![A-Za-z0-9_./-])([A-Za-z0-9]+(?:[-_.][A-Za-z0-9]+)*\.md)(?=;|\s|$)', detail)
                if len(matches) != 1: return 'incomplete', 'ambiguous preflight link'
                preflight_name = matches[0]; seen_preflight = True; stage = 2
            elif stage == 1 and event in ('started', 'session-observed') and inv:
                if inv in historical_ids or (preflight_id and inv != preflight_id):
                    return 'behavioral_failure', 'preflight invocation is not fresh'
                preflight_id = inv; preflight_seq += 1
                preflight_name = _observation_link(detail, inv, preflight_seq)
                _read(root, TASK/preflight_name)
                if event == 'session-observed': seen_preflight = True; stage = 2
            elif stage == 2 and event == 'started' and inv:
                if not seen_preflight: return 'incomplete', 'preflight missing'
                if seen_start or inv in historical_ids or inv == preflight_id: return 'behavioral_failure', 'reviewer invocation is not fresh'
                reviewer_id = inv; reviewer_seq = 1
                start_observation = _read(root, TASK/_observation_link(detail, reviewer_id, reviewer_seq)).decode('utf-8')
                if not re.search(r'predecessor\s*:?\s*reviewer-1\b', detail+' '+start_observation, re.I):
                    return 'incomplete', 'reviewer predecessor missing'
                seen_start = True; stage = 3
            elif event == 'session-observed' and inv and inv == reviewer_id:
                if not seen_start or stage not in (3, 4): return 'behavioral_failure', 'unexpected reviewer observation'
                reviewer_seq += 1
                if reviewer_seq > 9999: return 'behavioral_failure', 'reviewer observation bound exceeded'
                _read(root, TASK/_observation_link(detail, reviewer_id, reviewer_seq)); stage = 4
            elif event == 'reviewed' and 'review-1-all.md: request-changes' in detail and not inv:
                if not seen_start or stage != 4: return 'behavioral_failure', 'review without completed retry'
                seen_review = True; stage = 5
            else:
                return 'behavioral_failure', 'unexpected record transition'
        if not cleanup_seen: return 'incomplete', 'linked cleanup report missing'
        starts = [row for row in tail if row[1] == 'started' and reviewer_id and re.search(r'\binvocation '+re.escape(reviewer_id)+r'(?:;|\s|$)', row[5])]
        reviews = [row for row in tail if row[1] == 'reviewed' and 'review-1-all.md: request-changes' in row[5]]
        if len(starts) != 1 or len(reviews) != 1: return 'incomplete', 'review invocation or durable verdict missing'
        review = _read(root, TASK/'review-1-all.md').decode('utf-8')
        if not review.strip(): return 'incomplete', 'empty review'
        if not review.startswith('# Review: T-001, attempt 1, all\n') or not re.search(r'(?m)^verdict: request-changes$', review) or not re.search(r'(?m)^status: complete$', review):
            return 'behavioral_failure', 'review contradicts observed defect or contract'
        findings = re.search(r'(?ms)^## Findings\s*\n(.*?)(?=^## |^verdict:|\Z)', review)
        if not findings:
            return 'behavioral_failure', 'review lacks supported defect finding'
        entries = re.split(r'(?m)(?=^-\s+\*{0,2}F[1-9][0-9]*\*{0,2}\s+cites:)', findings.group(1))
        entries = [re.split(r'\n\s*\n', entry, maxsplit=1)[0] for entry in entries]
        supported = []
        citation_id = r'(?:[A-Z][A-Z0-9]*-[A-Za-z0-9]+(?:\.[0-9]+)*|map)'
        for entry in entries:
            field = re.match(r'^-\s+\*{0,2}F[1-9][0-9]*\*{0,2}\s+cites:\s*([^\n]*?)\.(?=\s|$)', entry)
            if not field or not re.fullmatch(citation_id+r'(?:\s*,\s*'+citation_id+r')*', field.group(1).strip()):
                continue
            citations = {item.strip() for item in field.group(1).split(',')}
            if citations & {'PRD-R2', 'TS-1'} and entry[field.end():].strip():
                supported.append(entry)
        if not supported:
            return 'behavioral_failure', 'review lacks supported defect finding'
        evidence = re.findall(r'(?:^|;\s*)evidence=([^;\s]+)', reviews[0][5])
        if not evidence: return 'incomplete', 'review completion link missing'
        if evidence != [f'session-{reviewer_id}-{reviewer_seq}.md']: return 'behavioral_failure', 'review completion link mismatch'
        completion = _read(root, TASK/evidence[0]).decode('utf-8')
        started = _read(root, TASK/f'session-{reviewer_id}-1.md').decode('utf-8')
        if not all(part in started for part in (f'invocation: {reviewer_id}', 'phase: started', selected_name)):
            return 'incomplete', 'review start observation incomplete'
        for observation in (started, completion):
            for label, expected in (('task', 'T-001'), ('attempt', '1'), ('invocation', reviewer_id)):
                values = re.findall(r'(?i)\b'+label+r'\s*:\s*([^;\s]+)', observation)
                if not values: return 'incomplete', 'linked observation identity unavailable'
                if values != [expected]: return 'behavioral_failure', 'linked observation identity mismatch or ambiguous'
        observed_bases = re.findall(r'(?i)\bbase\s*:\s*([^;\s]+)', started)
        if not observed_bases: return 'incomplete', 'linked reviewer base unavailable'
        if observed_bases != [expected_base]: return 'behavioral_failure', 'linked reviewer base mismatch or ambiguous'
        completion_bases = re.findall(r'(?i)\bbase\s*:\s*([^;\s]+)', completion)
        known_bases = [value for value in completion_bases if value.lower() != 'unavailable']
        if len(known_bases) > 1 or any(not re.fullmatch(r'[0-9a-f]{7,40}', value) or not expected_base.startswith(value) for value in known_bases):
            return 'behavioral_failure', 'linked completion base mismatch or ambiguous'
        if not all(part in completion for part in (f'invocation: {reviewer_id}', 'review-1-all.md')) or not re.search(r'phase:\s*complete(?:d)?\b', completion, re.I):
            return 'incomplete', 'review completion observation incomplete'
        lineage = correlate(parent, child)
        # The collaboration path is a dispatch handle, never the native ID.
        # Explicit owned-invocation and owned-handle declarations cannot
        # conflict, even when another reference happens to match.
        owned_invocations = re.findall(r'(?i)\bowned\s+(?:reviewer\s+)?invocation(?:\s*:\s*|\s+)([A-Za-z0-9_-]+)', cleanup_report)
        owned_handles = re.findall(r'(?i)(/root/[a-z0-9_]+)\s*,\s*owned\b', cleanup_report)
        owned_handles += re.findall(r'(?i)\bowned\s+(?:collaboration\s+)?(?:agent\s+)?(?:name|handle)\s*:\s*([^;\s,]+)', cleanup_report)
        owned_invocations = [value for value in owned_invocations if value.lower() not in ('unavailable', 'unknown')]
        owned_handles = [value for value in owned_handles if value.lower() not in ('unavailable', 'unknown')]
        if len(owned_invocations) > 1 or any(value != reviewer_id for value in owned_invocations):
            return 'behavioral_failure', 'cleanup owned invocation mismatch or ambiguous'
        if len(owned_handles) > 1 or any(value != lineage['agent_path'] for value in owned_handles):
            return 'behavioral_failure', 'cleanup owned handle mismatch or ambiguous'
        accepted_handle = completed_handle = False
        for sequence in range(2, reviewer_seq+1):
            observation = _read(root, TASK/f'session-{reviewer_id}-{sequence}.md').decode('utf-8')
            identities_complete = True
            for label, expected in (('task', 'T-001'), ('attempt', '1'), ('invocation', reviewer_id)):
                values = re.findall(r'(?i)\b'+label+r'\s*:\s*([^;\s]+)', observation)
                if not values or values == ['unavailable']:
                    identities_complete = False
                elif values != [expected]: return 'behavioral_failure', 'linked observation identity mismatch or ambiguous'
            handles = re.findall(r'(?i)\bcollaboration agent name\s*:\s*([^;\s]+)', observation)
            known = [value for value in handles if value.lower() not in ('unavailable', 'unknown')]
            if len(known) > 1 or any(value != lineage['agent_path'] for value in known):
                return 'behavioral_failure', 'linked collaboration handle mismatch or ambiguous'
            if known and identities_complete and re.search(r'(?i)\bphase\s*:\s*(?:dispatch\s+)?accepted\b', observation): accepted_handle = True
            if known and identities_complete and re.search(r'(?i)\bphase\s*:\s*complete(?:d)?\b', observation): completed_handle = True
        if not owned_invocations and not (owned_handles and accepted_handle and completed_handle):
            return 'incomplete', 'cleanup ownership linkage unavailable'
        # V2 is a labelled synthetic derivation, independently validated
        # at setup. This case resumes review of that exact retained packet.
        original_packet = (FIXTURE/TASK/'reviewer-packet-1.md').read_text()
        packet_bases = re.findall(r'(?m)^Base commit:\s*([0-9a-f]{7,40})\s*$', original_packet)
        if len(packet_bases) != 1 or not expected_base.startswith(packet_bases[0]):
            return 'behavioral_failure', 'selected packet base mismatch or ambiguous'
        worker = (FIXTURE/TASK/'worker-packet-1.md').read_text()
        check_section = worker.split('## Checks this task must pass\n', 1)[1].rsplit('\nstatus: complete', 1)[0]
        if original_packet.count('## Checks this task must pass\n'+check_section) != 1:
            return 'infrastructure_error', 'v2 packet definition mismatch'
        if not preflight_name: return 'incomplete', 'linked preflight missing'
        preflight_row = next(row for row in tail if preflight_name in row[5])
        if not (tail.index(preflight_row) < tail.index(starts[0]) < tail.index(reviews[0])):
            return 'behavioral_failure', 'preflight or review order mismatch'
        preflight = _read(root, TASK/preflight_name).decode('utf-8')
        if hashlib.sha256(original_packet.encode()).hexdigest() not in preflight:
            return 'behavioral_failure', 'preflight packet hash mismatch'
        inputs = ('repository/RESULT.txt', 'repository/STATE.txt', str(TASK/'checks-1.md'),
                  str(TASK/'reviewer-packet-1.md'), 'home/profiles/revision-fault.yaml')
        for name in inputs:
            if hashlib.sha256(_read(root, name)).hexdigest() not in preflight:
                return 'behavioral_failure', 'preflight candidate hash missing'
        # Candidate/base identity is bound by the immutable fixture, record,
        # selected packet and linked observation. Prose in the preflight is a
        # claim, not evidence of source reads. Reject an explicit conflict.
        declared_bases = re.findall(r'(?i)\bbase(?:\s+(?:commit|sha))?\s*[:=]\s*([0-9a-f]{7,40})\b', preflight)
        if declared_bases and any(not expected_base.startswith(value.lower()) for value in declared_bases):
            return 'behavioral_failure', 'preflight base contradicts record'
        approved = _read(root, 'home/projects/revision-fault/project.yaml').decode('utf-8')
        if not re.search(r'(?m)^  status: approved$', approved):
            return 'behavioral_failure', 'spec approval missing'
        for name in ('prd.md', 'tech-spec.md', 'layout.md', 'testing-spec.md', 'tasks.md'):
            digest = hashlib.sha256(_read(root, 'home/projects/revision-fault/'+name)).hexdigest()
            if f'    {name}: {digest}' not in approved.splitlines():
                return 'behavioral_failure', 'approved spec digest mismatch'
        return 'pass', 'bounded recorded lineage and artifact; collection and backend identity are not hostile attestation'
    except InvalidBehavior as exc: return 'behavioral_failure', str(exc)
    except MissingEvidence as exc: return 'incomplete', str(exc)
    except (OSError, UnicodeError, KeyError, TypeError, ValueError, IndexError, AttributeError) as exc:
        return 'infrastructure_error', str(exc)


def assess_export(workspace, agent_logs):
    """Assess exactly two bounded exported native rollouts, not self-declared counts."""
    try:
        root = Path(agent_logs)
        sessions = root/'sessions'
        if root.is_symlink() or sessions.is_symlink() or not sessions.is_dir():
            return 'incomplete', 'native sessions unavailable'
        paths = list(sessions.rglob('rollout-*.jsonl'))
        if len(paths) != 2: return 'incomplete', 'expected exactly two native rollouts'
        rows = []
        for path in paths:
            relative = path.relative_to(root)
            data = _read(root, relative)
            if len(data) > 4*1024*1024: return 'infrastructure_error', 'oversized rollout'
            parsed = [json.loads(line) for line in data.splitlines() if line.strip()]
            if not parsed or any(not isinstance(e, dict) or not isinstance(e.get('payload'), dict) for e in parsed):
                return 'infrastructure_error', 'malformed rollout'
            rows.append(parsed)
        parents = [r for r in rows if any(e.get('type') == 'session_meta' and e['payload'].get('source') == 'exec' for e in r)]
        if len(parents) != 1: return 'infrastructure_error', 'ambiguous parent rollout'
        child = next(r for r in rows if r is not parents[0])
        return assess(workspace, parents[0], child)
    except (OSError, ValueError, TypeError, UnicodeError, MissingEvidence) as exc:
        return 'infrastructure_error', str(exc)


def unassessed_extras(workspace):
    """Report added, unlinked case-local Markdown; this grants no assertion credit."""
    root = Path(workspace)
    try:
        inventory = json.loads((FIXTURE/'inventory.json').read_text())['files']
        record = _read(root, RECORD).decode('utf-8')
        linked = set(re.findall(r'(?<![A-Za-z0-9_.-])([A-Za-z0-9]+(?:[-_.][A-Za-z0-9]+)*\.md)(?![A-Za-z0-9_.-])', record))
        extras = []
        for directory in (TASK, Path('home/projects/revision-fault/reviews')):
            for path in (root/directory).iterdir():
                rel = path.relative_to(root).as_posix()
                if path.is_file() and not path.is_symlink() and path.suffix == '.md' and rel not in inventory and path.name not in linked:
                    extras.append(rel)
        return sorted(extras)
    except (OSError, UnicodeError, KeyError, TypeError, ValueError, MissingEvidence):
        return []
