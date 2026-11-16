"""Bounded filename grammar for task observations and run cleanup reports."""
import re


def safe_id(value):
    return isinstance(value, str) and len(value) <= 128 and re.fullmatch(r'[A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*', value) is not None


def parse_name(name, prefix):
    if not isinstance(name, str) or prefix not in ('session', 'cleanup'):
        return None
    match = re.fullmatch(re.escape(prefix)+r'-([A-Za-z0-9]+(?:[-_][A-Za-z0-9]+)*)-([1-9][0-9]{0,3})\.md', name)
    if match is None or not safe_id(match.group(1)):
        return None
    return match.group(1), int(match.group(2))
