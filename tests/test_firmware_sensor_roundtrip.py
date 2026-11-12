"""Actual C++ full-app -> authenticated Go/SQLite -> actual C++ ACK, host only."""
from pathlib import Path
import configparser
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'


class FirmwareSensorRoundtrip(unittest.TestCase):
    def test_named_opt_in_environment_keeps_normal_default_off(self):
        config = configparser.ConfigParser(interpolation=None)
        config.read(FW / 'platformio.ini')
        self.assertEqual(config['platformio']['default_envs'].strip(), 'sigap_n16r8')
        self.assertNotIn('SIGAP_SENSOR_SIMULATION=1', config['env:sigap_n16r8']['build_flags'])
        self.assertIn('env:sigap_n16r8_sensor_sim', config)
        selected = config['env:sigap_n16r8_sensor_sim']
        self.assertEqual(selected['extends'], 'env:sigap_n16r8')
        self.assertIn('${env:sigap_n16r8.build_flags}', selected['build_flags'])
        self.assertIn('-DSIGAP_SENSOR_SIMULATION=1', selected['build_flags'])

    def test_actual_full_app_and_backend_ack_roundtrip(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            binary = Path(directory) / 'producer'
            host = FW / 'test/host/debug_app'
            includes = [host / 'include', host, FW / 'include', FW / 'src/app']
            includes += sorted((FW / 'lib').glob('*/src'))
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DARDUINO',
                       '-DSIGAP_DEBUG_APP_HOST_TEST', '-DSIGAP_SENSOR_SIMULATION=1']
            command += [f'-Wno-{warning}' for warning in (
                'unused-parameter', 'unused-variable', 'unused-but-set-variable',
                'unused-function', 'class-memaccess', 'switch', 'reorder', 'sign-compare')]
            command += [f'-I{path}' for path in includes]
            command += [str(FW / 'test/host/sensor_simulation/archive_probe.cpp')]
            command += [str(FW / 'src/app' / name) for name in (
                'sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
            command += [str(path) for path in sorted((FW / 'lib').glob('*/src/**/*.cpp'))]
            command += ['-o', str(binary)]
            subprocess.run(command, cwd=ROOT, check=True, timeout=180)
            artifacts = Path(os.environ.get('SIGAP_SENSOR_TEST_ARTIFACTS', str(Path(directory) / 'artifacts')))
            artifacts.mkdir(exist_ok=False)
            environment = dict(os.environ, SIGAP_SENSOR_TEST_PRODUCER=str(binary),
                               SIGAP_SENSOR_TEST_ARTIFACTS=str(artifacts),
                               GOTOOLCHAIN='local', GOPROXY='off')
            go = os.environ.get('SIGAP_GO_TEST_BIN', 'go')
            subprocess.run([go, 'test', '-mod=readonly', './internal/http', '-run',
                            '^TestFirmwareSensorRoundtrip$', '-count=1', '-v'],
                           cwd=ROOT, env=environment, check=True, timeout=240)


if __name__ == '__main__':
    unittest.main()
