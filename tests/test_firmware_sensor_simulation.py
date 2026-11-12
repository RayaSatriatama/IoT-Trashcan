"""Host execution of the actual ARDUINO-shaped sensor composition, not HIL."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
HOST = FW / 'test/host/sensor_simulation'

class SensorSimulation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binaries = {}
        includes = [HOST / 'include', FW / 'include'] + sorted((FW / 'lib').glob('*/src'))
        cls.includes = includes
        for mode in (0, 1):
            binary = Path(cls.temp.name) / f'source-{mode}'
            command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror', '-DARDUINO',
                       f'-DSIGAP_SENSOR_SIMULATION={mode}']
            command += [f'-I{path}' for path in includes]
            command += [str(HOST / 'probe.cpp'), '-o', str(binary)]
            subprocess.run(command, cwd=ROOT, check=True, timeout=60)
            cls.binaries[mode] = binary

    def run_case(self, mode, case):
        subprocess.run([str(self.binaries[mode]), case], cwd=ROOT, check=True, timeout=15)

    def test_simulated_graph_produces_all13_without_physical_io(self):
        self.run_case(1, 'sim')

    def test_off_uses_original_physical_readers_and_build_tag(self):
        self.run_case(0, 'off')

    def test_actual_physical_hx711_signed24_boundaries(self):
        self.run_case(0, 'signed')

    def test_invalid_source_flag_is_rejected(self):
        command = ['g++', '-std=c++17', '-DARDUINO', '-DSIGAP_SENSOR_SIMULATION=2', '-fsyntax-only']
        command += [f'-I{path}' for path in self.includes]
        command += [str(HOST / 'probe.cpp')]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(result.returncode, 0, 'invalid simulation flag was silently accepted')

if __name__ == '__main__':
    unittest.main()
