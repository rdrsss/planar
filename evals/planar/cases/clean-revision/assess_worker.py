"""Structural first-step boundary only; host capture and audit are prerequisites."""
import re

BASE = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'
TASK = 'home/projects/revision-fault/tasks/T-001/'

class Incomplete(ValueError): pass
class Contradiction(ValueError): pass


def assess_worker(files):
    def read(name):
        value = files.get(name)
        if not isinstance(value, bytes): raise Incomplete('missing boundary file')
        try: return value.decode('utf-8')
        except UnicodeDecodeError: raise Incomplete('non-UTF-8 boundary evidence') from None
    record = read(TASK+'record.md')
    if not record.startswith('---\n') or '\n---\n' not in record[4:] or '\n## Log\n' not in record:
        raise Incomplete('record structure missing')
    head = record[4:].split('\n---\n', 1)[0].splitlines()
    required = {'task':'T-001', 'state':'worker-done', 'attempt':'1', 'base':BASE, 'profile':'revision-fault', 'rung':'1'}
    for key, expected in required.items():
        values = [line[len(key)+1:].strip() for line in head if line.startswith(key+':')]
        if not values: raise Incomplete('record field missing')
        if values != [expected]: raise Contradiction('record field mismatch')
    rows = [line.split(' | ') for line in record.split('\n## Log\n',1)[1].splitlines() if line.startswith('- ')]
    if not rows or any(len(row) != 6 for row in rows): raise Incomplete('record log missing or malformed')
    for row in rows:
        if row[2] != 'attempt 1': raise Contradiction('unexpected attempt')
        if row[1] not in ('started', 'session-observed', 'packet-rendered', 'worker-done'):
            raise Contradiction('unexpected first-step transition')
        if re.search(r'(?i)\b(?:checks?|review(?:er)?)(?:[-_]|\b)', row[5]):
            raise Contradiction('check or review activity before boundary')
    done = [row for row in rows if row[1] == 'worker-done']
    if len(done) != 1 or 'worker-report-1.md' not in done[0][5]:
        raise Incomplete('worker completion link missing or ambiguous')
    if rows.index(done[0]) != len(rows)-1 and any(row[1] != 'session-observed' for row in rows[rows.index(done[0])+1:]):
        raise Contradiction('activity after worker completion')
    starts = [row for row in rows if row[1] == 'started' and row[3].startswith('worker ')]
    if len(starts) != 1: raise Incomplete('worker start missing or ambiguous')
    found = re.findall(r'\binvocation ([A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*)', starts[0][5])
    if len(found) != 1: raise Incomplete('worker invocation missing')
    invocation = found[0]
    first = f'session-{invocation}-1.md'
    if first not in starts[0][5]: raise Incomplete('worker start observation link missing')
    evidence = re.findall(r'(?:^|;\s*)evidence=([^;\s]+)', done[0][5])
    if len(evidence) != 1 or not re.fullmatch(r'session-'+re.escape(invocation)+r'-(?:[2-9]|[1-9][0-9]+)\.md', evidence[0]):
        raise Incomplete('worker completion observation link missing')
    for filename, phase in ((first, 'started'), (evidence[0], 'completion')):
        observation = read(TASK+filename)
        for field in (f'invocation: {invocation}', 'task: T-001', 'attempt: 1', 'worker-packet-1.md', 'worker-report-1.md', f'phase: {phase}'):
            if field not in observation: raise Incomplete('worker observation incomplete')
    read(TASK+'worker-packet-1.md')
    report = read(TASK+'worker-report-1.md')
    if report.splitlines()[-1:] != ['status: done']: raise Incomplete('worker report incomplete')
    if files.get('repository/RESULT.txt') != b'2\n' or files.get('repository/STATE.txt') != b'correct\n':
        raise Contradiction('worker output bytes mismatch')
    if re.search(r'(?m)^- \[x\] \*\*T-001\*\*', read('home/projects/revision-fault/tasks.md'), re.I):
        raise Contradiction('task already ticked')
    if any(name.startswith(TASK+prefix) for name in files for prefix in ('checks-', 'review-', 'reviewer-packet-', 'session-check', 'session-review')):
        raise Contradiction('check or review artifact already present')
    return {'boundary':'worker-done', 'attempt':1, 'rung':1}
