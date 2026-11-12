"""Actual new app configuration on fake SDK, not physical HIL."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
class LiveApplication(unittest.TestCase):
    def test_actual_application_split_queue_and_ack_wiring(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            binary = Path(directory) / 'live-app'
            host = FW / 'test/host/debug_app'
            includes = [host/'include', host, FW/'include', FW/'src/app']
            includes += sorted((FW/'lib').glob('*/src'))
            command = ['g++', '-std=c++17', '-DARDUINO', '-DSIGAP_DEBUG_APP_HOST_TEST',
                       '-DSIGAP_LIVE_ARCHIVE_SPLIT=1', '-DSIGAP_PHYSICAL_REFERENCE_ONLY=1',
                       '-DSIGAP_SENSOR_SIMULATION=0']
            command += [f'-I{p}' for p in includes]
            command += [str(FW/'test/host/reclaim_workspace/live_app.cpp')]
            command += [str(FW/'src/app'/n) for n in
                        ('sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
            command += [str(p) for p in sorted((FW/'lib').glob('*/src/**/*.cpp'))]
            command += ['-o', str(binary)]
            subprocess.run(command, cwd=ROOT, check=True, timeout=180)
            subprocess.run([str(binary)], cwd=ROOT, check=True, timeout=30)
if __name__ == '__main__': unittest.main()
