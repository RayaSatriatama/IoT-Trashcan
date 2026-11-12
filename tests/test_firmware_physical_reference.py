"""Actual four-reference-sensor graph with GPIO fixture, not hardware acceptance."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
class PhysicalReference(unittest.TestCase):
    def test_four_physical_channels_exclude_all_other_gpio_and_heater(self):
        with tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR')) as directory:
            binary = Path(directory) / 'probe'
            includes = [FW/'test/host/physical_reference/include', FW/'test/host/sensor_simulation/include', FW/'include']
            includes += sorted((FW/'lib').glob('*/src'))
            cmd = ['g++','-std=c++17','-Wall','-Wextra','-Werror','-DARDUINO','-DSIGAP_SENSOR_SIMULATION=0','-DSIGAP_PHYSICAL_REFERENCE_ONLY=1']
            cmd += ['-I'+str(p) for p in includes]
            cmd += [str(FW/'test/host/physical_reference/probe.cpp'),'-o',str(binary)]
            subprocess.run(cmd,cwd=ROOT,check=True,timeout=60)
            subprocess.run([str(binary)],cwd=ROOT,check=True,timeout=15)
if __name__ == '__main__':
    unittest.main()
