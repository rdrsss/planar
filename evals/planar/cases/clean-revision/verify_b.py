"""Bounded structural revision oracle; semantic audit remains mandatory."""
import hashlib
import json
from pathlib import Path
import re
from assess_worker import Incomplete, Contradiction, TASK
from boundary import verify_injection
from capture import load
from chronology import receipt, order, within_native
from native import correlate, MissingEvidence, InvalidBehavior
from verify_a import assess as assess_first

BASE = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'
CHECK = r"""test -f RESULT.txt && test "$(od -An -tx1 RESULT.txt | tr -d ' \n')" = 320a"""


def _text(snapshot, path):
    raw = snapshot.get(path)
    if not isinstance(raw, bytes): raise Incomplete('missing required artifact')
    try: return raw.decode('utf-8')
    except UnicodeDecodeError: raise Incomplete('invalid artifact encoding') from None


def _rows(record):
    if '\n## Log\n' not in record: raise Incomplete('record log missing')
    rows = [line.split(' | ') for line in record.split('\n## Log\n',1)[1].splitlines() if line.startswith('- ')]
    if not rows or any(len(row) != 6 for row in rows): raise Incomplete('record log malformed')
    return rows


def _rollouts(path, expected):
    if path.is_symlink() or not path.is_dir(): raise MissingEvidence('native logs missing')
    found = list(path.rglob('rollout-*.jsonl'))
    if len(found) < expected: raise MissingEvidence('native rollout missing')
    if len(found) != expected: raise InvalidBehavior('unexpected native rollout')
    parents, children = [], []
    for file in found:
        if file.is_symlink() or file.stat().st_size > 4*1024*1024: raise MissingEvidence('unsafe rollout')
        try: rows = [json.loads(line) for line in file.read_text().splitlines()]
        except (ValueError, UnicodeError): raise MissingEvidence('invalid rollout') from None
        metas = [x for x in rows if isinstance(x,dict) and x.get('type') == 'session_meta']
        if len(metas) != 1 or not isinstance(metas[0].get('payload'),dict): raise MissingEvidence('rollout identity missing')
        if metas[0]['payload'].get('source') == 'exec': parents.append(rows)
        else: children.append(rows)
    if len(parents) != 1: raise MissingEvidence('parent ambiguous')
    # Role order is identified from structured spawn paths, not filesystem order.
    def child_start(rows):
        meta = next(x for x in rows if x.get('type') == 'session_meta')
        return meta.get('timestamp','')
    return parents[0], sorted(children,key=child_start)


def assess(host_root):
    root = Path(host_root)
    try:
        first = assess_first(root)
        if first['status'] != 'pass': return first
        pre_time, post_time, final_time = (receipt(root,name) for name in ('pre','post','final'))
        injection_path = root/'injection.json'
        if injection_path.is_symlink() or not injection_path.is_file(): raise Incomplete('injection receipt missing')
        injection = json.loads(injection_path.read_text())
        started, ended = injection['started'], injection['ended']
        order(pre_time,started); order(started,ended); order(ended,post_time); order(post_time,final_time)
        pre, post, final = (load(root/name) for name in ('pre','post','final'))
        old = json.loads(pre['host/repository-inventory.json'])
        new = json.loads(post['host/repository-inventory.json'])
        old_state = old['files'].pop('STATE.txt', None); new_state = new['files'].pop('STATE.txt', None)
        def state_meta(value):
            return {'sha256':hashlib.sha256(value).hexdigest(), 'size':len(value)}
        if old_state != state_meta(pre.get('repository/STATE.txt',b'')) or new_state != state_meta(post.get('repository/STATE.txt',b'')) or old != new:
            raise Contradiction('injection metadata changed')
        verify_injection({k:v for k,v in pre.items() if k != 'host/repository-inventory.json'},
                         {k:v for k,v in post.items() if k != 'host/repository-inventory.json'})
        for name, data in pre.items():
            if name.startswith(TASK) and name != TASK+'record.md' and final.get(name) != data:
                raise Contradiction('earlier task evidence changed')
        if final.get('repository/RESULT.txt') != b'2\n' or final.get('repository/STATE.txt') != b'correct\n':
            raise Contradiction('final output mismatch')
        if final.get('home/projects/revision-fault/tasks.md') != pre.get('home/projects/revision-fault/tasks.md'):
            raise Contradiction('task checkbox changed')
        record = _text(final,TASK+'record.md'); original = _text(pre,TASK+'record.md')
        if not record.startswith('---\n') or '\n---\n' not in record[4:]: raise Incomplete('record frontmatter missing')
        head = record[4:].split('\n---\n',1)[0].splitlines()
        for key, value in {'task':'T-001','state':'reviewed','attempt':'2','rung':'1','profile':'revision-fault','base':BASE}.items():
            fields = [line[len(key)+1:].strip() for line in head if line.startswith(key+':')]
            if not fields: raise Incomplete('record field missing')
            if fields != [value]: raise Contradiction('record field mismatch')
        if not record.split('\n## Log\n',1)[-1].startswith(original.split('\n## Log\n',1)[-1]):
            raise Contradiction('earlier record log changed')
        rows = _rows(record)
        allowed = {'started','session-observed','packet-rendered','worker-done','checks-done','review-packet-rendered','reviewed','changes-requested'}
        if any(row[1] not in allowed for row in rows): raise Contradiction('unexpected record event')
        core = [(r[1],r[2]) for r in rows if r[1] in ('packet-rendered','worker-done','checks-done','review-packet-rendered','reviewed','changes-requested','approved','blocked','question')]
        expected = [('packet-rendered','attempt 1'),('worker-done','attempt 1'),('checks-done','attempt 1'),('review-packet-rendered','attempt 1'),('reviewed','attempt 1'),('changes-requested','attempt 1'),('packet-rendered','attempt 2'),('worker-done','attempt 2'),('checks-done','attempt 2'),('review-packet-rendered','attempt 2'),('reviewed','attempt 2')]
        if core != expected: raise Contradiction('record transitions mismatch')
        if any(row[2] not in ('attempt 1','attempt 2') for row in rows): raise Contradiction('unexpected attempt')
        for attempt in (1,2):
            checks = _text(final,TASK+f'checks-{attempt}.md')
            if not checks.startswith(f'# Checks: T-001, attempt {attempt}\n') or '\n## TEST-C1\n' not in checks or '\nResult: passed\n' not in checks or '\nExit code: 0\n' not in checks or ('Command: `'+CHECK+'`') not in checks or checks.splitlines()[-1:] != ['status: complete']:
                raise Contradiction('check artifact mismatch')
        for step, filename in (('packet-rendered','worker-packet-2.md'),('worker-done','worker-report-2.md'),('checks-done','checks-1.md'),('checks-done','checks-2.md'),('review-packet-rendered','reviewer-packet-1.md'),('review-packet-rendered','reviewer-packet-2.md'),('reviewed','review-1-all.md'),('reviewed','review-2-all.md')):
            if sum(row[1] == step and filename in row[5] for row in rows) != 1: raise Incomplete('artifact link missing or ambiguous')
        review = _text(final,TASK+'review-1-all.md')
        if not review.startswith('# Review: T-001, attempt 1, all\n') or not review.endswith('verdict: request-changes\nstatus: complete\n'):
            raise Contradiction('first review mismatch')
        entries = re.findall(r'(?ms)^-\s+\*\*F[1-9][0-9]*\*\*\s+(?:\(note\)\s+)?cites:\s*[^\n]*?\.(?=\s|$).*?(?=\n\s*\n|\nverdict:|\Z)', review)
        numbered = re.findall(r'(?m)^-\s+\*\*F[1-9][0-9]*\*\*', review)
        if len(numbered) != len(entries): raise Contradiction('malformed numbered finding')
        findings = [entry for entry in entries if re.match(r'^-\s+\*\*F[1-9][0-9]*\*\*\s+cites:\s*(?:[^.]*,\s*)?(?:PRD-R2|TS-1)(?:\s*,[^.]*)?\.',entry)]
        if not findings: raise Contradiction('numbered cited finding missing')
        if not any(all(word in finding.lower() for word in ('state', 'fault-injected', 'correct')) for finding in findings):
            raise Contradiction('finding lacks actual state and expected correction')
        worker_packet = _text(final,TASK+'worker-packet-2.md')
        if not all(finding in worker_packet for finding in entries): raise Contradiction('actual finding not carried')
        if worker_packet.splitlines()[-1:] != ['status: complete']:
            raise Incomplete('revision packet incomplete')
        for attempt in (1,2):
            if _text(final,TASK+f'reviewer-packet-{attempt}.md').splitlines()[-1:] != ['status: complete']:
                raise Incomplete('review packet incomplete')
        if _text(final,TASK+'worker-report-2.md').splitlines()[-1:] != ['status: done']:
            raise Incomplete('revision worker incomplete')
        review2 = _text(final,TASK+'review-2-all.md')
        if not review2.startswith('# Review: T-001, attempt 2, all\n') or not review2.endswith('verdict: approve\nstatus: complete\n'):
            raise Contradiction('second review mismatch')
        p1,c1 = _rollouts(root/'logs/worker',2); p2,c2 = _rollouts(root/'logs/revision',4)
        lineage = correlate([p1,p2],c1+c2)
        within_native(post_time,p2,final_time)
        return {'status':'pass','lineage':lineage,'semantic_audit':'required'}
    except (Incomplete, MissingEvidence, FileNotFoundError, OSError, UnicodeError, json.JSONDecodeError, KeyError):
        return {'status':'incomplete'}
    except (Contradiction, InvalidBehavior, ValueError):
        return {'status':'behavioral_failure'}
