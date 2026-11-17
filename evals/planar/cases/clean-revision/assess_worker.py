"""Structural first-step boundary only; host capture and audit are prerequisites."""
import re
import hashlib

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
        values = [m.group(1).strip() for line in head if (m := re.fullmatch(re.escape(key)+r':\s*(.*)', line, re.I))]
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
    # Only the two documented result-field link forms are recognized.  The
    # observation itself must agree; filenames alone are never proof.
    link = re.compile(r'(?:\binvocation\s+([A-Za-z0-9_-]+)\s*;\s*|\b([A-Za-z0-9_-]+):\s*)(session-([A-Za-z0-9_-]+)-([1-9][0-9]*)\.md)')
    def links(row):
        result = []
        for m in link.finditer(row[5]):
            identity = m.group(1) or m.group(2)
            if identity != m.group(4): raise Contradiction('observation link identity mismatch')
            result.append((identity, m.group(3), int(m.group(5))))
        return result
    def observation_fields(filename):
        fields = {}
        for line in read(TASK+filename).splitlines():
            for part in line.lstrip('- ').split(';'):
                match = re.match(r'^\s*([A-Za-z][A-Za-z ]*):\s*(.*?)\s*$', part)
                if not match: continue
                key = match.group(1).lower()
                if key in ('task','attempt','step','role','invocation','parent','phase','packet','result','base'):
                    if key in fields: raise Contradiction('duplicate observation field')
                    fields[key] = match.group(2)
        return fields
    packets = [row for row in rows if row[1] == 'packet-rendered']
    if len(packets) != 1: raise Incomplete('packet rendering missing or ambiguous')
    packet_index = rows.index(packets[0])
    drivers = [row for row in rows[:packet_index] if row[1] == 'started' and not row[3].startswith('worker ')]
    if len(drivers) != 1: raise Incomplete('packet driver start missing or ambiguous')
    driver_links = links(drivers[0])
    if len(driver_links) != 1 or driver_links[0][2] != 1:
        raise Incomplete('packet driver observation link missing or ambiguous')
    driver, driver_file, _ = driver_links[0]
    fields = observation_fields(driver_file)
    expected = {'task':'T-001', 'attempt':'1', 'step':'packet-rendered', 'invocation':driver,
                'packet':'worker-packet-1.md', 'result':'worker-packet-1.md', 'base':BASE}
    for key, value in expected.items():
        if key not in fields: raise Incomplete('packet driver observation field missing')
        if fields[key] != value: raise Contradiction('packet driver observation field mismatch')
    if fields.get('role') not in ('driver', 'session driver') or fields.get('phase', '').split(',',1)[0].strip().lower() != 'started':
        raise Contradiction('packet driver observation role or phase mismatch')
    if packet_index >= rows.index(starts[0]): raise Contradiction('packet driver ordering')
    start_links = links(starts[0])
    if len(start_links) != 1 or start_links[0][2] != 1:
        raise Incomplete('worker start observation link missing or ambiguous')
    invocation, first, _ = start_links[0]
    if rows.index(starts[0]) >= rows.index(done[0]): raise Contradiction('worker start ordering')
    linked = [(rows.index(starts[0]), first, 1)]
    for index, row in enumerate(rows):
        if row[1] != 'session-observed': continue
        for identity, filename, number in links(row):
            if identity != invocation and row[3].startswith('worker '):
                raise Contradiction('conflicting worker invocation link')
            if identity == invocation:
                if not rows.index(starts[0]) < index < rows.index(done[0]):
                    raise Contradiction('worker observation ordering')
                linked.append((index, filename, number))
    if len({item[1] for item in linked}) != len(linked):
        raise Contradiction('duplicate worker observation link')
    numbers = [item[2] for item in linked]
    if numbers != list(range(1, len(numbers)+1)):
        raise Contradiction('worker observation sequence')
    present = {name[len(TASK):] for name in files if re.fullmatch(re.escape(TASK)+'session-'+re.escape(invocation)+r'-[1-9][0-9]*\.md', name)}
    if present != {item[1] for item in linked}:
        raise Contradiction('unlinked worker observation')
    report_bytes = files.get(TASK+'worker-report-1.md')
    if not isinstance(report_bytes, bytes): raise Incomplete('missing worker report')
    parent = None; completion = []
    for index, filename, number in linked:
        fields = observation_fields(filename)
        expected = {'task':'T-001', 'attempt':'1', 'step':'worker-done', 'role':'worker',
                    'invocation':invocation, 'packet':'worker-packet-1.md', 'base':BASE}
        for key, value in expected.items():
            if key not in fields: raise Incomplete('worker observation field missing')
            if fields[key] != value: raise Contradiction('worker observation field mismatch')
        if 'parent' not in fields or not fields['parent']: raise Incomplete('worker parent missing')
        if fields['parent'] != driver and not re.fullmatch(r'unavailable(?: \([^)]*\))?', fields['parent'], re.I):
            raise Contradiction('worker parent does not match packet driver')
        if parent is None: parent = fields['parent']
        elif parent != fields['parent']: raise Contradiction('worker parent mismatch')
        if 'result' not in fields: raise Incomplete('worker result missing')
        result = fields['result']
        if not re.fullmatch(r'worker-report-1\.md(?:, SHA-256 [0-9a-f]{64}, status: done)?', result, re.I):
            raise Contradiction('worker result mismatch')
        if ',' in result and hashlib.sha256(report_bytes).hexdigest() not in result:
            raise Contradiction('worker report digest mismatch')
        phase = fields.get('phase', '').split(',',1)[0].strip().lower()
        if number == 1:
            if phase != 'started': raise Contradiction('worker start phase mismatch')
        elif phase in ('completion', 'completed'):
            completion.append(filename)
        elif phase != 'dispatch accepted':
            raise Contradiction('worker observation phase mismatch')
    if len(completion) != 1: raise Incomplete('worker completion observation missing or ambiguous')
    evidence = re.findall(r'(?:^|;\s*)evidence=([^;\s]+)', done[0][5])
    if len(evidence) > 1 or (evidence and evidence[0] != completion[0]):
        raise Contradiction('worker completion evidence conflict')
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
