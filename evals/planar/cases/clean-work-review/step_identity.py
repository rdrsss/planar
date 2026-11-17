"""Identify Harbor steps from frozen, host-read instruction bytes.

This recognizes step identity only; it is not proof of a passed prior gate,
quiescence, snapshot preservation, or permission to inject.
"""
import hashlib
from pathlib import Path

NAMES = ('worker', 'review')


def load_instructions(case):
    root = Path(case)
    result = {}
    for name in NAMES:
        path = root / 'steps' / name / 'instruction.md'
        if path.is_symlink() or not path.is_file():
            raise ValueError('missing or unsafe instruction')
        raw = path.read_bytes()
        try:
            decoded = raw.decode('utf-8')
        except UnicodeDecodeError:
            raise ValueError('instruction is not UTF-8') from None
        if not decoded:
            raise ValueError('empty instruction')
        result[name] = (decoded, hashlib.sha256(raw).hexdigest())
    if result['worker'][0] == result['review'][0]:
        raise ValueError('ambiguous instructions')
    return result


def identify(instruction, frozen):
    if not isinstance(instruction, str) or set(frozen) != set(NAMES):
        raise ValueError('invalid step input')
    matches = [name for name in NAMES if instruction == frozen[name][0] and
               hashlib.sha256(instruction.encode('utf-8')).hexdigest() == frozen[name][1]]
    if len(matches) != 1:
        raise ValueError('instruction does not match one frozen step')
    return matches[0]


def validate_selection(frozen, model, effort):
    """Bind each factual instruction statement to the actual selected config."""
    if model != 'gpt-6.1-terra' or effort != 'medium' or set(frozen) != set(NAMES):
        raise ValueError('clean work/review configured selection mismatch')
    statement = (f'The launcher selected this fresh session with model `{model}` and reasoning effort `{effort}` '
                 'in its fixed job configuration. This is the configured selection; backend model and effort are not independently observed.')
    if any(frozen[name][0].count(statement) != 1 for name in NAMES):
        raise ValueError('clean work/review instruction selection mismatch')
    return statement
