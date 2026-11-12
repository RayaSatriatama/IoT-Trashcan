"""Actual full-app host composition, fake SDK persistence only; not HIL."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
HOST = FW / 'test/host/debug_app'

class SensorSimulationApp(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'sensor-simulation-app'
        includes = [HOST / 'include', HOST, FW / 'include', FW / 'src/app']
        includes += sorted((FW / 'lib').glob('*/src'))
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-DARDUINO',
                   '-DSIGAP_DEBUG_APP_HOST_TEST', '-DSIGAP_SENSOR_SIMULATION=1',
                   '-DSIGAP_NETWORK_ENDPOINT="https://host-test.invalid"',
                   '-DSIGAP_NETWORK_CA_CERT="HOST_TEST_CA"',
                   '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
                   '-Wno-unused-function', '-Wno-class-memaccess', '-Wno-switch',
                   '-Wno-reorder', '-Wno-sign-compare']
        command += [f'-I{path}' for path in includes]
        command += [str(FW / 'test/host/sensor_simulation/app_probe.cpp')]
        command += [str(FW / 'src/app' / name) for name in
                    ('sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
        command += [str(path) for path in sorted((FW / 'lib').glob('*/src/**/*.cpp'))]
        command += ['-o', str(cls.binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=180)

    def test_normal_simulated_app_has_explicit_protocol_profile(self):
        subprocess.run([str(self.binary), 'profile'], cwd=ROOT, check=True, timeout=15)

    def test_mq7_archive_preserves_each_heater_controller_phase(self):
        subprocess.run([str(self.binary), 'phase'], cwd=ROOT, check=True, timeout=15)

    def test_actual_host_transport_adapters_reject_simulated_persistence(self):
        subprocess.run([str(self.binary), 'transports'], cwd=ROOT, check=True, timeout=15)

    def test_common_router_blocks_real_calibration_persistence_in_simulation(self):
        subprocess.run([str(self.binary), 'calibration'], cwd=ROOT, check=True, timeout=15)

if __name__ == '__main__':
    unittest.main()
