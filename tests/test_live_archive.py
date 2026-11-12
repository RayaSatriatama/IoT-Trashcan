"""Production active queue against finite host FS; not device HIL."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
FW=ROOT/'firmware'
class LiveArchive(unittest.TestCase):
    def test_isolated_queue_progresses_without_modifying_retained_archive(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            binary=Path(directory)/'live-queue'
            includes=[FW/'test/host/reclaim_workspace',FW/'include',*sorted((FW/'lib').glob('*/src'))]
            cmd=['g++','-std=c++17','-Wall','-Wextra','-DARDUINO=1',*['-I'+str(p)for p in includes],str(FW/'test/host/reclaim_workspace/live_queue.cpp'),'-o',str(binary)]
            subprocess.run(cmd,cwd=ROOT,check=True,timeout=60)
            subprocess.run([str(binary)],cwd=ROOT,check=True,timeout=20)
if __name__=='__main__':unittest.main()
