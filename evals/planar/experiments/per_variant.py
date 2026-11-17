"""TS-25.8 closed portable input and template boundary; no authorization side effects."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat

SLOTS = ('candidate.json', 'execution.json', 'tests/bundle/manifest.json', 'tests/bundle/execution.json')
HEX = re.compile(r'[0-9a-f]{64}\Z')
CONTRACT = {'case': 'planar/native-lineage-portable', 'contract_version': 2, 'slots': list(SLOTS)}
PLACEHOLDER = b'unbound\n'
ID = re.compile(r'[a-zA-Z0-9][a-zA-Z0-9_-]{0,63}\Z')

class Invalid(ValueError):
    pass

def canonical(value):
    return json.dumps(value, ensure_ascii=True, sort_keys=True, separators=(',', ':')).encode('utf-8') + b'\n'

def digest(data):
    return hashlib.sha256(data).hexdigest()

def safe_id(value):
    if not isinstance(value, str) or not ID.fullmatch(value):
        raise Invalid('unsafe identifier')
    return value

def parse(data, limit):
    if not isinstance(data, bytes) or len(data) > limit:
        raise Invalid('input size')
    try:
        value = json.loads(data)
    except (ValueError, UnicodeDecodeError):
        raise Invalid('invalid JSON') from None
    if canonical(value) != data:
        raise Invalid('noncanonical JSON')
    return value

def validate_inventory(data):
    obj = parse(data, 1024 * 1024)
    if not isinstance(obj, dict) or set(obj) != {'algorithm', 'sha256', 'files'} or obj['algorithm'] != 'sha256-json-inventory-v1':
        raise Invalid('inventory schema')
    files = obj['files']
    if not isinstance(files, list) or not files:
        raise Invalid('empty inventory')
    paths = []
    for item in files:
        if not isinstance(item, dict) or set(item) != {'path', 'sha256'}:
            raise Invalid('inventory entry')
        path, sha = item['path'], item['sha256']
        if not isinstance(path, str) or '\\' in path or '\0' in path or any(part in ('', '.', '..') for part in path.split('/')):
            raise Invalid('unsafe inventory path')
        if not isinstance(sha, str) or not HEX.fullmatch(sha):
            raise Invalid('invalid file hash')
        paths.append(path)
    if paths != sorted(set(paths)) or obj['sha256'] != digest(canonical(files)[:-1]):
        raise Invalid('inventory ordering or digest')
    return obj

def validate_execution(data):
    obj = parse(data, 4096)
    if not isinstance(obj, dict) or set(obj) != {'schema_version', 'driver', 'child'} or type(obj['schema_version']) is not int or obj['schema_version'] != 1:
        raise Invalid('execution schema')
    for key in ('driver', 'child'):
        binding = obj[key]
        if not isinstance(binding, dict) or set(binding) != {'model', 'effort'} or binding['model'] != 'gpt-6.1-terra' or binding['effort'] not in (('medium', 'high') if key == 'driver' else ('medium',)):
            raise Invalid('unsupported execution binding')
    return obj

def tree(root):
    root = Path(root)
    if root.is_symlink() or not root.is_dir():
        raise Invalid('unsafe tree root')
    result = {}
    def walk(directory):
        for entry in sorted(os.scandir(directory), key=lambda e: e.name):
            path = Path(entry.path)
            relative = path.relative_to(root).as_posix()
            mode = entry.stat(follow_symlinks=False).st_mode
            if stat.S_ISDIR(mode):
                result[relative] = ('dir', None)
                walk(path)
            elif stat.S_ISREG(mode):
                result[relative] = ('file', digest(path.read_bytes()))
            else:
                raise Invalid('unsafe tree entry')
    walk(root)
    return result

def inventory(root):
    entries = [{'path': path, 'sha256': value} for path, (kind, value) in tree(root).items() if kind == 'file']
    entries.sort(key=lambda e: e['path'])
    obj = {'algorithm':'sha256-json-inventory-v1', 'files':entries, 'sha256':digest(canonical(entries)[:-1])}
    data = canonical(obj)
    validate_inventory(data)
    return data

def tree_hash(root):
    return digest(canonical(tree(root)))

def verifier_code_hash(instance):
    entries = tree(Path(instance) / 'tests')
    return digest(canonical({path: value for path, value in entries.items()
                             if path not in ('bundle/manifest.json', 'bundle/execution.json')}))

def validate_template(template, expected_template_sha256):
    """Require a caller-supplied reviewed pin; never derive authorization from input."""
    if not isinstance(expected_template_sha256, str) or not HEX.fullmatch(expected_template_sha256):
        raise Invalid('reviewed template pin required')
    template = Path(template)
    entries = tree(template)
    if tree_hash(template) != expected_template_sha256:
        raise Invalid('template identity mismatch')
    if entries.get('portable-contract.json', (None,))[0] != 'file' or (template / 'portable-contract.json').read_bytes() != canonical(CONTRACT):
        raise Invalid('unknown portable contract or slots')
    for slot in SLOTS:
        if entries.get(slot, (None,))[0] != 'file' or (template / slot).read_bytes() != PLACEHOLDER:
            raise Invalid('invalid template placeholder')
    return entries

def validate_instance(template, instance, candidate, *, expected_template_sha256):
    template, instance = Path(template), Path(instance)
    before, after = validate_template(template, expected_template_sha256), tree(instance)
    if set(before) != set(after):
        raise Invalid('template paths differ')
    for path in before:
        if before[path][0] != after[path][0] or (path not in SLOTS and before[path] != after[path]):
            raise Invalid('template type or bytes differ')
    if any(before.get(slot, (None,))[0] != 'file' for slot in SLOTS):
        raise Invalid('missing regular slot')
    i = (instance / SLOTS[0]).read_bytes()
    e = (instance / SLOTS[1]).read_bytes()
    if i != (instance / SLOTS[2]).read_bytes() or e != (instance / SLOTS[3]).read_bytes():
        raise Invalid('divergent trusted copies')
    iv, ev = validate_inventory(i), validate_execution(e)
    if i != inventory(candidate):
        raise Invalid('candidate inventory mismatch')
    return {'case_template_sha256':tree_hash(template), 'case_instance_sha256':tree_hash(instance), 'candidate_inventory_sha256':iv['sha256'], 'verifier_code_sha256':verifier_code_hash(instance), 'trusted_inputs_sha256':digest(canonical({'inventory':iv, 'execution':ev}))}

def materialize(template, destination, candidate, execution, *, expected_template_sha256):
    """Create a new instance only; existing destinations are never overwritten."""
    template, destination = Path(template), Path(destination)
    validate_template(template, expected_template_sha256)
    i = inventory(candidate)
    e = canonical(execution)
    validate_execution(e)
    if destination.exists() or destination.is_symlink():
        raise Invalid('destination exists')
    shutil.copytree(template, destination, symlinks=False)
    try:
        for slot in SLOTS:
            (destination / slot).write_bytes(i if slot in (SLOTS[0], SLOTS[2]) else e)
        return validate_instance(template, destination, candidate, expected_template_sha256=expected_template_sha256)
    except BaseException:
        shutil.rmtree(destination)
        raise
