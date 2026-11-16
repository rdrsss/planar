"""No-model, no-container Harbor environment for control tests only."""
from harbor.environments.base import BaseEnvironment, ExecResult


class FakeEnvironment(BaseEnvironment):
    @staticmethod
    def type():
        return 'planar-control-fake'

    def _validate_definition(self):
        pass

    async def start(self, force_build=False):
        pass

    async def stop(self, delete=True):
        pass

    async def upload_file(self, source_path, target_path):
        pass

    async def upload_dir(self, source_dir, target_dir):
        pass

    async def download_file(self, source_path, target_path):
        raise FileNotFoundError(source_path)

    async def download_dir(self, source_dir, target_dir):
        pass

    async def exec(self, command, cwd=None, env=None, timeout_sec=None, user=None):
        return ExecResult(return_code=0, stdout='', stderr='')
