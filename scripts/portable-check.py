#!/usr/bin/env python3
"""Inspect distribution binaries and prove their CLI startup is usable."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile

BINARIES = ('planar', 'planar-agent', 'planar-watch', 'planar-execute', 'planar-ext')
GLIBC_LIBS = {'libc.so.6', 'libm.so.6', 'libpthread.so.0', 'libdl.so.2',
              'librt.so.1', 'libresolv.so.2', 'libutil.so.1', 'libanl.so.1',
              'ld-linux-x86-64.so.2', 'ld-linux-aarch64.so.1'}


def run(command, env=None):
    result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=30)
    if result.returncode:
        raise ValueError(f"{command[0]} exited {result.returncode}: {result.stderr.strip()}")
    return result.stdout


def inspect_macos(dependencies, commands, prefix):
    """Mach-O permits system frameworks/libSystem, no rpath, minos exactly 26.0."""
    if prefix in dependencies or prefix in commands:
        raise ValueError(f'toolchain prefix leaked: {prefix}')
    if re.search(r'\bcmd LC_RPATH\b', commands):
        raise ValueError('LC_RPATH present: ' + commands[commands.index('LC_RPATH'):].split('Load command', 1)[0].strip())
    libraries = re.findall(r'^\s+(\S+) \(compatibility version', dependencies, re.M)
    if not libraries:
        raise ValueError('no Mach-O dependencies found')
    for library in libraries:
        if not (library == '/usr/lib/libSystem.B.dylib' or
                library.startswith('/System/Library/Frameworks/') or
                library.startswith('/System/Library/PrivateFrameworks/')):
            raise ValueError(f'unexpected shared library: {library}')
        if re.search(r'lib(c\+\+|c\+\+abi|unwind|ssl|crypto)', library):
            raise ValueError(f'shared runtime/TLS library: {library}')
    builds = re.findall(r'cmd LC_BUILD_VERSION\b(.*?)(?=Load command|\Z)', commands, re.S)
    floors = [re.search(r'\bminos\s+(\S+)', build) for build in builds]
    if not floors or any(floor is None or floor[1] != '26.0' for floor in floors):
        found = [floor[1] if floor else 'missing' for floor in floors]
        raise ValueError(f'LC_BUILD_VERSION minos {found or "missing"}; expected 26.0')


def inspect_linux(dynamic, versions, prefix):
    """ELF permits only glibc companions, no search path, GLIBC at most 2.36."""
    if prefix in dynamic or prefix in versions:
        raise ValueError(f'toolchain prefix leaked: {prefix}')
    if re.search(r'\((?:RUNPATH|RPATH)\)', dynamic):
        raise ValueError('RUNPATH/RPATH present: ' + '\n'.join(line.strip() for line in dynamic.splitlines() if re.search(r'\((?:RUNPATH|RPATH)\)', line)))
    libraries = re.findall(r'\(NEEDED\).*?\[([^\]]+)\]', dynamic)
    if not libraries or 'libc.so.6' not in libraries:
        raise ValueError('missing glibc NEEDED entry')
    for library in libraries:
        if library not in GLIBC_LIBS:
            raise ValueError(f'unexpected shared library: {library}')
    required = re.findall(r'\bGLIBC_([0-9]+(?:\.[0-9]+)+)\b', versions)
    if not required:
        raise ValueError('no GLIBC symbol versions found')
    maximum = max(tuple(map(int, version.split('.'))) for version in required)
    if maximum > (2, 36):
        raise ValueError(f'GLIBC_{".".join(map(str, maximum))} exceeds GLIBC_2.36')
    if re.search(r'\bGLIBC_PRIVATE\b', versions):
        raise ValueError('GLIBC_PRIVATE dependency')


def check(bin_dir, platform, prefix):
    with tempfile.TemporaryDirectory(prefix='planar-portable-') as scratch:
        env = dict(os.environ, HOME=scratch, PLANAR_DB=str(Path(scratch) / 'planar.db'),
                   PLANAR_CONFIG_PATH=str(Path(scratch) / 'config.toml'))
        for name in BINARIES:
            binary = bin_dir / name
            print(f'inspecting {name}: {binary}', flush=True)
            try:
                if platform == 'Darwin':
                    inspect_macos(run(['otool', '-L', str(binary)]),
                                  run(['otool', '-l', str(binary)]), prefix)
                else:
                    inspect_linux(run(['readelf', '-d', '--wide', str(binary)]),
                                  run(['readelf', '--version-info', '--wide', str(binary)]), prefix)
                run([str(binary), '--help'], env)
            except (ValueError, OSError, subprocess.TimeoutExpired) as error:
                raise ValueError(f'{name}: {error}') from error
            print(f'{name}: dependencies, floor and startup passed', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bin-dir', type=Path, required=True)
    parser.add_argument('--platform', choices=('Darwin', 'Linux'), required=True)
    parser.add_argument('--toolchain-prefix', required=True)
    args = parser.parse_args()
    try:
        check(args.bin_dir, args.platform, args.toolchain_prefix)
    except ValueError as error:
        print(f'portability check failed: {error}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
