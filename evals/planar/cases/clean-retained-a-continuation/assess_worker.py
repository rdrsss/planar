"""Worker boundary using task-local record and observation links."""
from links import TASK, Incomplete, Contradiction, text, record, observations, linked_start


def assess_worker(files):
    _, header, rows = record(files)
    if header.get('state') != 'worker-done': raise Contradiction('wrong first boundary state')
    allowed = {'started', 'session-observed', 'packet-rendered', 'worker-done'}
    if any(row[1] not in allowed for row in rows): raise Contradiction('activity after first boundary')
    packet = [i for i, row in enumerate(rows) if row[1] == 'packet-rendered' and 'worker-packet-1.md' in row[5]]
    done = [i for i, row in enumerate(rows) if row[1] == 'worker-done' and 'worker-report-1.md' in row[5]]
    if len(packet) != 1 or len(done) != 1: raise Incomplete('worker artifacts not linked')
    obs = observations(files, rows)
    start, group = linked_start(files, obs, rows, 'worker', 'worker-packet-1.md', 'worker-report-1.md')
    if not packet[0] < start[0] < done[0]: raise Contradiction('worker ordering mismatch')
    phase = lambda v: v[3].get('phase', '').split(',', 1)[0].strip().lower()
    if phase(start) not in ('start', 'started'): raise Incomplete('worker start phase missing')
    if not any(phase(v) in ('accepted', 'dispatch accepted') for v in group): raise Incomplete('worker acceptance missing')
    completed = [v for v in group if phase(v) in ('completed', 'completion')]
    if len(completed) != 1 or completed[0][0] >= done[0]: raise Incomplete('worker completion missing')
    if not any(v[1] == start[3]['parent'] and v[2] == 1 and v[0] < packet[0] for v in obs.values()):
        raise Incomplete('worker parent link missing')
    text(files, TASK+'worker-packet-1.md')
    if text(files, TASK+'worker-report-1.md').splitlines()[-1:] != ['status: done']: raise Incomplete('worker report incomplete')
    if files.get('repository/RESULT.txt') != b'2\n' or files.get('repository/STATE.txt') != b'correct\n': raise Contradiction('worker products mismatch')
    if '- [ ] **T-001**' not in text(files, 'home/projects/revision-fault/tasks.md'): raise Contradiction('task already approved')
    if any(name.startswith(TASK+prefix) for name in files for prefix in ('checks-', 'review-', 'reviewer-packet-')): raise Contradiction('check or review before boundary')
    return {'boundary':'worker-done', 'attempt':1, 'rung':1}
