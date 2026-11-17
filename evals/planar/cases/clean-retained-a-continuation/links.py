"""Read task-local invocation links without depending on result-field prefixes."""
import hashlib
import re
from datetime import datetime

TASK = 'home/projects/revision-fault/tasks/T-001/'
BASE = '08bacc341a31f32c14a1695b5b2de5c44b3a3e56'


class Incomplete(ValueError): pass
class Contradiction(ValueError): pass


def text(files, name):
    raw = files.get(name)
    if not isinstance(raw, bytes): raise Incomplete('missing artifact: ' + name)
    try: return raw.decode('utf-8')
    except UnicodeDecodeError: raise Incomplete('invalid artifact encoding') from None


def check_base(files, value):
    """Accept only a hexadecimal prefix of the one verified frozen base."""
    if not re.fullmatch(r'[0-9a-f]{7,40}',value) or not BASE.startswith(value):
        raise Contradiction('base identity mismatch')
    if len(value)<40 and files.get('host/git-head.txt')!=(BASE+'\n').encode():
        raise Incomplete('full host base unavailable')


def normalize_field(key, value):
    if key=='base':
        match=re.fullmatch(r'([0-9a-f]{7,40})(?:\.(?:\s+Profile: .*)?)?',value)
        if not match: raise Contradiction('invalid base field')
        return match.group(1)
    if key=='parent':
        match=re.fullmatch(r'([A-Za-z0-9_-]+) \(same driver\)',value)
        return match.group(1) if match else value
    if key=='phase':
        value=value.split(',',1)[0].strip().removesuffix('.').lower()
        if value.startswith('accepted. collaboration name: '): value='accepted'
        if value=='started. packet: unavailable (final tree check)': value='started'
        return 'completed' if value=='complete' else value
    if key=='timestamp':
        value=value.removesuffix('.')
        try:
            if datetime.fromisoformat(value.replace('Z','+00:00')).utcoffset() is None: raise ValueError()
        except ValueError: raise Incomplete('invalid observation time') from None
    return value


def record(files):
    value = text(files, TASK+'record.md')
    if not value.startswith('---\n') or '\n---\n' not in value[4:] or '\n## Log\n' not in value:
        raise Incomplete('record structure missing')
    header = {}
    for line in value[4:].split('\n---\n', 1)[0].splitlines():
        if ':' not in line: continue
        key, val = line.split(':', 1); key = key.strip().lower()
        if key in header: raise Contradiction('duplicate frontmatter field')
        header[key] = val.strip()
    for key, expected in {'task':'T-001', 'attempt':'1', 'rung':'1', 'profile':'revision-fault'}.items():
        if key not in header: raise Incomplete('frontmatter missing ' + key)
        if header[key] != expected: raise Contradiction('frontmatter mismatch ' + key)
    if 'base' not in header: raise Incomplete('frontmatter missing base')
    check_base(files,header['base'])
    rows = [line.split(' | ') for line in value.split('\n## Log\n', 1)[1].splitlines() if line.startswith('- ')]
    if not rows or any(len(row) != 6 for row in rows): raise Incomplete('malformed log')
    if any(row[2] != 'attempt 1' for row in rows): raise Contradiction('unexpected attempt')
    return value, header, rows


SESSION = re.compile(r'(?<![\w/.-])(session-([A-Za-z0-9_-]+)-([1-9][0-9]*)\.md)(?![\w/.-])')


def observations(files, rows):
    """Join links by filename, then compare their declared identity and task."""
    found = {}
    for index, row in enumerate(rows):
        for match in SESSION.finditer(row[5]):
            filename, invocation, number = match.group(1), match.group(2), int(match.group(3))
            if filename in found:
                first=found[filename][0]
                if (first == index or rows[first][1]!='session-observed'
                    or invocation!=found[filename][1] or found[filename][3].get('phase') not in ('completed','completion') or not ((row[1]=='worker-done' and 'worker-report-1.md' in row[5]) or (row[1]=='reviewed' and 'review-1-all.md' in row[5]))):
                    raise Contradiction('duplicate session link')
                continue
            data = text(files, TASK+filename)
            fields = {}
            for line in data.splitlines():
                for part in line.lstrip('- ').split(';'):
                    m = re.match(r'^\s*([A-Za-z][A-Za-z ]*):\s*(.*?)\s*$', part)
                    if not m: continue
                    key = m.group(1).lower()
                    if key == 'invocation id': key = 'invocation'
                    if key == 'time': key = 'timestamp'
                    if key not in ('task', 'attempt', 'step', 'role', 'invocation', 'parent', 'phase', 'packet', 'result', 'base', 'timestamp'): continue
                    if key in fields: raise Contradiction('duplicate observation field')
                    fields[key] = normalize_field(key,m.group(2))
            for key, expected in (('task', 'T-001'), ('attempt', '1'), ('invocation', invocation)):
                if key not in fields: raise Incomplete('observation field missing')
                if fields[key] != expected: raise Contradiction('observation identity mismatch')
            if 'base' not in fields: raise Incomplete('observation field missing')
            check_base(files,fields['base'])
            found[filename] = (index, invocation, number, fields)
    for invocation in {v[1] for v in found.values()}:
        group = sorted((v for v in found.values() if v[1] == invocation), key=lambda v: v[0])
        if [v[2] for v in group] != list(range(1, len(group)+1)): raise Contradiction('observation sequence mismatch')
        present = {name[len(TASK):] for name in files if re.fullmatch(re.escape(TASK)+'session-'+re.escape(invocation)+r'-[1-9][0-9]*\.md', name)}
        linked = {name for name, v in found.items() if v[1] == invocation}
        if present != linked: raise Contradiction('unlinked invocation observation')
    all_present = {name[len(TASK):] for name in files if re.fullmatch(re.escape(TASK)+r'session-[A-Za-z0-9_-]+-[1-9][0-9]*\.md', name)}
    if all_present != set(found): raise Contradiction('unlinked session observation')
    return found


def linked_start(files, obs, rows, role, packet, result):
    """Require a linked initial observation and consistent linked continuation."""
    starts = [v for v in obs.values() if v[2] == 1 and rows[v[0]][1] == 'started' and v[3].get('role', '').lower() == role]
    if len(starts) != 1: raise Incomplete('role start missing or ambiguous')
    start = starts[0]; fields = start[3]
    for key in ('step', 'parent', 'phase', 'packet', 'result', 'timestamp'):
        if not fields.get(key): raise Incomplete('start field missing')
    if fields['packet'] != packet or not fields['result'].startswith(result): raise Contradiction('start artifact mismatch')
    group = sorted((v for v in obs.values() if v[1] == start[1]), key=lambda v: v[0])
    for v in group:
        f = v[3]
        for key in ('role', 'parent', 'packet', 'step'):
            if f.get(key) != fields[key]: raise Contradiction('invocation observation conflict')
        if not f.get('result', '').startswith(result): raise Contradiction('invocation result conflict')
        digest = re.search(r'SHA-256\s+([0-9a-f]{64})', f['result'], re.I)
        if digest and hashlib.sha256(files.get(TASK+result, b'')).hexdigest() != digest.group(1).lower(): raise Contradiction('result digest mismatch')
    return start, group
