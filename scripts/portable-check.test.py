#!/usr/bin/env python3
"""Fixtures pin the portability contract independently of the build host."""
import importlib.util
from pathlib import Path
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('portable', Path(__file__).with_name('portable-check.py'))
portable = importlib.util.module_from_spec(spec)
spec.loader.exec_module(portable)
PREFIX = '/toolchain/llvm'
MAC_LIBS = '\t/usr/lib/libSystem.B.dylib (compatibility version 1.0.0, current version 1.0.0)\n'
MAC_BUILD = 'Load command 1\n      cmd LC_BUILD_VERSION\n platform 1\n    minos 26.0\n'
ELF_LIBS = ' (NEEDED) Shared library: [libm.so.6]\n (NEEDED) Shared library: [libc.so.6]\n'
ELF_VERSIONS = 'Name: GLIBC_2.2.5\nName: GLIBC_2.36\n'


class PortableContract(unittest.TestCase):
    def test_startup_failure_names_binary(self):
        def fake_run(command, env=None):
            if command[0] == 'otool':
                return MAC_LIBS if command[1] == '-L' else MAC_BUILD
            if command[0].endswith('planar-ext'):
                raise ValueError('CLI exited -6: invalid free during startup')
            self.assertIsNotNone(env)
            self.assertNotEqual(env['HOME'], str(Path.home()))
            return 'help'
        with patch.object(portable, 'run', side_effect=fake_run):
            with self.assertRaisesRegex(ValueError, 'planar-ext: CLI exited -6'):
                portable.check(Path('/fixture/bin'), 'Darwin', PREFIX)

    def test_macos_positive(self):
        portable.inspect_macos(MAC_LIBS, MAC_BUILD, PREFIX)

    def test_linux_positive(self):
        portable.inspect_linux(ELF_LIBS + ' (NEEDED) [ld-linux-x86-64.so.2]', ELF_VERSIONS, PREFIX)

    def test_macos_leaks_and_floor(self):
        for dependencies, commands, diagnostic in (
            (MAC_LIBS, MAC_BUILD + 'cmd LC_RPATH\npath /tmp/elsewhere', 'LC_RPATH'),
            (MAC_LIBS, MAC_BUILD + PREFIX, 'toolchain prefix'),
            (MAC_LIBS + '\t/usr/lib/libc++.1.dylib (compatibility version 1.0.0)', MAC_BUILD, 'libc++'),
            (MAC_LIBS, MAC_BUILD.replace('26.0', '27.0'), '27.0'),
            (MAC_LIBS, '', 'missing'),
        ):
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(ValueError, diagnostic):
                portable.inspect_macos(dependencies, commands, PREFIX)

    def test_linux_leaks_and_floor(self):
        for dynamic, versions, diagnostic in (
            (ELF_LIBS + ' (RUNPATH) [/tmp/elsewhere]', ELF_VERSIONS, 'RUNPATH'),
            (ELF_LIBS + ' (RPATH) [/tmp/elsewhere]', ELF_VERSIONS, 'RPATH'),
            (ELF_LIBS + PREFIX, ELF_VERSIONS, 'toolchain prefix'),
            (ELF_LIBS, ELF_VERSIONS + 'Name: GLIBC_2.38', '2.38'),
            (ELF_LIBS, ELF_VERSIONS + 'Name: GLIBC_2.100', '2.100'),
            (ELF_LIBS, '', 'no GLIBC'),
            ('', ELF_VERSIONS, 'missing glibc'),
        ):
            with self.subTest(diagnostic=diagnostic), self.assertRaisesRegex(ValueError, diagnostic):
                portable.inspect_linux(dynamic, versions, PREFIX)
        for library in ('libc++.so.1', 'libc++abi.so.1', 'libunwind.so.1',
                        'libgcc_s.so.1', 'libssl.so.3', 'libcrypto.so.3', 'libzstd.so.1'):
            with self.subTest(library=library), self.assertRaises(ValueError):
                portable.inspect_linux(ELF_LIBS + f' (NEEDED) [{library}]', ELF_VERSIONS, PREFIX)


if __name__ == '__main__':
    unittest.main()
