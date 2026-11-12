"""Initialized actual-app adapter admission, dual SIM/OFF; fake SDK/FS only."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
HOST = FW / 'test/host/debug_app'


class SimulationAdmission(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        includes = [HOST / 'include', HOST, FW / 'include', FW / 'src/app']
        includes += sorted((FW / 'lib').glob('*/src'))
        for mode in (0, 1):
            binary = Path(cls.temp.name) / f'admission-{mode}'
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DARDUINO',
                       '-DSIGAP_DEBUG_APP_HOST_TEST', f'-DSIGAP_SENSOR_SIMULATION={mode}',
                       '-DSIGAP_NETWORK_ENDPOINT="https://host-test.invalid"',
                       '-DSIGAP_NETWORK_CA_CERT="HOST_TEST_CA"']
            command += ['-Wno-' + warning for warning in (
                'unused-parameter', 'unused-variable', 'unused-but-set-variable',
                'unused-function', 'class-memaccess', 'switch', 'reorder', 'sign-compare')]
            command += [f'-I{path}' for path in includes]
            command += [str(FW / 'test/host/sensor_simulation/sim_admission_probe.cpp')]
            command += [str(FW / 'src/app' / name) for name in (
                'sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
            command += [str(path) for path in sorted((FW / 'lib').glob('*/src/**/*.cpp'))]
            command += ['-o', str(binary)]
            result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=180)
            if result.returncode:
                raise RuntimeError(result.stdout.decode() + result.stderr.decode())
            cls.binaries[mode] = binary

    def run_case(self, mode, scenario):
        result = subprocess.run([str(self.binaries[mode]), scenario], cwd=ROOT,
                                capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, (result.stdout + result.stderr).decode())

    def test_off_usb_start_and_cancel_positive(self):
        self.run_case(0, 'usb')

    def test_sim_usb_start_preserves_business_state(self):
        self.run_case(1, 'usb')

    def test_off_ble_start_and_cancel_positive(self):
        self.run_case(0, 'ble')

    def test_sim_ble_start_preserves_business_state(self):
        self.run_case(1, 'ble')

    def test_off_http_codec_executor_start_and_cancel_positive(self):
        self.run_case(0, 'http')

    def test_sim_http_denial_repeat_and_replay_state_preserved(self):
        self.run_case(1, 'http')


if __name__ == '__main__':
    unittest.main()
