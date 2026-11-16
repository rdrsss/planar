import asyncio
import io
import tarfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

from guarded_docker import GuardedDocker
from safe_export import MAX_ARCHIVE, select_native_logs


class Producer:
    def __init__(self, chunks):
        self.stdout = asyncio.StreamReader()
        self.returncode = None
        self.killed = False
        self.task = asyncio.create_task(self.feed(chunks))

    async def feed(self, chunks):
        for chunk in chunks:
            self.stdout.feed_data(chunk)
            await asyncio.sleep(0.001)
        self.stdout.feed_eof()
        self.returncode = 0

    async def wait(self):
        await self.task
        return self.returncode

    def kill(self):
        self.killed = True
        self.returncode = -9
        self.task.cancel()
        async def done():
            return None
        self.task = asyncio.create_task(done())


class StreamControls(unittest.IsolatedAsyncioTestCase):
    async def collect(self, chunks):
        process = Producer(chunks)
        agent = GuardedDocker.__new__(GuardedDocker)
        async def resolve(service): return 'test-container'
        agent._platform = SimpleNamespace(_resolve_service_container=resolve)
        async def spawn(*args, **kwargs): return process
        with patch('guarded_docker.asyncio.create_subprocess_exec', new=spawn):
            result = await agent._archive('/logs/agent/.')
        return result, process

    async def test_delayed_split_and_over_pipe_buffer(self):
        chunks = [b'a' * 512, b'b' * 1024, b'c' * 200000, b'd' * 11]
        data, process = await self.collect(chunks)
        self.assertEqual(data, b''.join(chunks))
        self.assertEqual(process.stdout._buffer, bytearray())
        self.assertFalse(process.killed)

    async def test_oversized_stream_kills_producer(self):
        producer = Producer([b'x' * 65536] * (MAX_ARCHIVE // 65536 + 1))
        agent = GuardedDocker.__new__(GuardedDocker)
        async def resolve(service): return 'test-container'
        agent._platform = SimpleNamespace(_resolve_service_container=resolve)
        async def spawn(*args, **kwargs): return producer
        with patch('guarded_docker.asyncio.create_subprocess_exec', new=spawn):
            with self.assertRaisesRegex(ValueError, 'exceeds'):
                await agent._archive('/logs/agent/.')
        self.assertTrue(producer.killed)

    async def test_late_forbidden_member_is_rejected(self):
        buffer = io.BytesIO()
        with tarfile.open(fileobj=buffer, mode='w') as archive:
            first = tarfile.TarInfo('codex.txt'); first.size = 200000
            archive.addfile(first, io.BytesIO(b'x' * first.size))
            late = tarfile.TarInfo('sessions/2026/11/16/rollout-evil.jsonl')
            late.type = tarfile.SYMTYPE; late.linkname = '/etc/passwd'
            archive.addfile(late)
        raw = buffer.getvalue()
        chunks = [raw[:512], raw[512:100000], raw[100000:200000], raw[200000:]]
        data, _ = await self.collect(chunks)
        self.assertEqual(data, raw)
        with self.assertRaises(ValueError):
            select_native_logs(data)
