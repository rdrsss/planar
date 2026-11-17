"""Bounded file-structure checks for fresh B; never grants a reward alone."""
import re
from native import MissingEvidence, InvalidBehavior
from packets import verify_reviewer_packet, _text, PROJECT, TASK
from check_evidence import CHECK


def _rows(record):
    if '\n## Log\n' not in record: raise MissingEvidence('record log missing')
    rows=[line.split(' | ') for line in record.split('\n## Log\n',1)[1].splitlines() if line.startswith('- ')]
    if not rows or any(len(row)!=6 for row in rows): raise MissingEvidence('record log malformed')
    return rows


def _check_report(text, attempt):
    if text.splitlines()[-1:] != ['status: complete']: raise MissingEvidence('check report incomplete')
    if f'Command: `{CHECK}`' not in text: raise MissingEvidence('check command not fully recorded')
    if not re.search(r'(?im)^\s*(?:working directory|cwd):\s*`?/workspace/repository`?\s*$',text):
        raise MissingEvidence('check working directory missing')
    if not re.search(r'(?im)^\s*(?:output|stdout):\s*(?:\(empty\)|\(none\)|empty|none|``)\s*$',text):
        raise MissingEvidence('check output missing')
    if not re.search(r'(?im)^\s*exit code:\s*0\s*$',text) or not re.search(r'(?im)^\s*result:\s*passed\s*$',text):
        raise MissingEvidence('check result or exit missing')


def verify(source, final):
    """Validate file evidence only; actual native check calls are separate."""
    original=_text(source,TASK+'record.md'); record=_text(final,TASK+'record.md')
    if not record.startswith('---\n') or '\n---\n' not in record[4:] or '\n## Log\n' not in record:
        raise MissingEvidence('final record malformed')
    head=record[4:].split('\n---\n',1)[0].splitlines()
    expected={'task':'T-001','state':'reviewed','attempt':'2','rung':'1','profile':'revision-fault','base':'08bacc341a31f32c14a1695b5b2de5c44b3a3e56'}
    for key,value in expected.items():
        entries=[line[len(key)+1:].strip() for line in head if line.startswith(key+':')]
        if not entries: raise MissingEvidence('final record field missing')
        if entries != [value]: raise InvalidBehavior('final record field mismatch')
    if not record.split('\n## Log\n',1)[1].startswith(original.split('\n## Log\n',1)[1]):
        raise InvalidBehavior('source log prefix rewritten')
    for name,data in source.items():
        if name.startswith('home/') and name != TASK+'record.md' and final.get(name)!=data:
            raise InvalidBehavior('source durable home artifact changed')
    if final.get(PROJECT+'tasks.md') != source.get(PROJECT+'tasks.md'):
        raise InvalidBehavior('task list changed')
    for name,value in (('repository/RESULT.txt',b'2\n'),('repository/STATE.txt',b'correct\n')):
        if final.get(name)!=value: raise InvalidBehavior('final output mismatch')
    for name in ('host/git-head.txt','host/git-stage.txt','host/git-status.txt'):
        if final.get(name)!=source.get(name): raise InvalidBehavior('final git boundary mismatch')
    rows=_rows(record)
    allowed={'started','session-observed','packet-rendered','worker-done','checks-done','review-packet-rendered','reviewed','changes-requested'}
    if any(row[1] not in allowed or row[2] not in ('attempt 1','attempt 2') for row in rows):
        raise MissingEvidence('unsupported target record transition')
    core=[(row[1],row[2]) for row in rows if row[1] in ('packet-rendered','worker-done','checks-done','review-packet-rendered','reviewed','changes-requested','approved','blocked','question')]
    expected_core=[('packet-rendered','attempt 1'),('worker-done','attempt 1'),('checks-done','attempt 1'),('review-packet-rendered','attempt 1'),('reviewed','attempt 1'),('changes-requested','attempt 1'),('packet-rendered','attempt 2'),('worker-done','attempt 2'),('checks-done','attempt 2'),('review-packet-rendered','attempt 2'),('reviewed','attempt 2')]
    if core!=expected_core: raise InvalidBehavior('revision transition mismatch')
    for attempt in (1,2):
        _check_report(_text(final,TASK+f'checks-{attempt}.md'),attempt)
        verify_reviewer_packet(source,final,attempt)
    for step,filename in (('packet-rendered','worker-packet-2.md'),('worker-done','worker-report-2.md'),('checks-done','checks-1.md'),('checks-done','checks-2.md'),('review-packet-rendered','reviewer-packet-1.md'),('review-packet-rendered','reviewer-packet-2.md'),('reviewed','review-1-all.md'),('reviewed','review-2-all.md')):
        if sum(row[1]==step and filename in row[5] for row in rows)!=1: raise MissingEvidence('artifact link missing or ambiguous')
    first=_text(final,TASK+'review-1-all.md')
    if not first.startswith('# Review: T-001, attempt 1, all\n') or not first.endswith('verdict: request-changes\nstatus: complete\n'): raise MissingEvidence('first review verdict unsupported')
    findings=re.findall(r'(?ms)^-\s+\*\*F[1-9][0-9]*\*\*\s+(?:\(note\)\s+)?cites:\s*[^\n]*?\.(?=\s|$).*?(?=\n\s*\n|\nverdict:|\Z)',first)
    numbered=re.findall(r'(?m)^-\s+\*\*F[1-9][0-9]*\*\*',first)
    if len(findings)!=len(numbered): raise MissingEvidence('unsupported finding format')
    actionable=[x for x in findings if re.match(r'^-\s+\*\*F[1-9][0-9]*\*\*\s+cites:\s*(?:[^.]*,\s*)?(?:PRD-R2|TS-1)(?:\s*,[^.]*)?\.',x)]
    if not any(all(word in x.lower() for word in ('state','fault-injected','correct')) for x in actionable):
        raise MissingEvidence('cited actionable state finding missing')
    worker_packet=_text(final,TASK+'worker-packet-2.md')
    if first.strip() not in worker_packet or any(x not in worker_packet for x in findings) or worker_packet.splitlines()[-1:] != ['status: complete']:
        raise MissingEvidence('revision findings not fully carried')
    if _text(final,TASK+'worker-report-2.md').splitlines()[-1:] != ['status: done']:
        raise MissingEvidence('revision worker report incomplete')
    second=_text(final,TASK+'review-2-all.md')
    if not second.startswith('# Review: T-001, attempt 2, all\n') or not second.endswith('verdict: approve\nstatus: complete\n'): raise MissingEvidence('fresh review verdict unsupported')
    return {'file_structure':'pass','native_check_proof':'required','semantic_audit':'required'}
