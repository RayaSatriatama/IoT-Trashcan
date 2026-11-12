"""Real SigapApp legacy selection and filesystem admission; SDK/storage are fakes."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
HOST = FW / 'test/host/debug_app'


class LegacyApplicationBinding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'legacy-actual-app'
        includes = [HOST / 'include', HOST, FW / 'include', FW / 'src/app']
        includes += sorted((FW / 'lib').glob('*/src'))
        command = ['g++', '-std=c++17', '-DARDUINO', '-DSIGAP_DEBUG_APP_HOST_TEST',
                   '-DSIGAP_SENSOR_SIMULATION=1',
                   '-DSIGAP_NETWORK_ENDPOINT="https://host-test.invalid"',
                   '-DSIGAP_NETWORK_CA_CERT="HOST_TEST_CA"']
        command += [f'-I{p}' for p in includes]
        command += [str(FW / 'test/host/sensor_simulation/legacy_app_probe.cpp')]
        command += [str(FW / 'src/app' / n) for n in
                    ('sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
        command += [str(p) for p in sorted((FW / 'lib').glob('*/src/**/*.cpp'))]
        command += ['-o', str(cls.binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=180)

    def test_actual_app_selects_eligible_after_historical_under_existing_lease(self):
        subprocess.run([str(self.binary), 'historical'], cwd=ROOT, check=True, timeout=15)

    def test_actual_app_does_not_skip_current_boot_producer_fault(self):
        subprocess.run([str(self.binary), 'current'], cwd=ROOT, check=True, timeout=15)

    def test_actual_app_rejects_canonical_foreign_device_before_batching(self):
        subprocess.run([str(self.binary), 'foreign'], cwd=ROOT, check=True, timeout=15)

    def test_actual_app_rejects_canonical_zero_boot_before_batching(self):
        subprocess.run([str(self.binary), 'zero_boot'], cwd=ROOT, check=True, timeout=15)


if __name__ == '__main__':
    unittest.main()
