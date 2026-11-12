from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware"
HOST = FW / "test/host/debug_http_transport"

class DebugHttpTransportHarness(unittest.TestCase):
    def test_production_translation_unit_with_controlled_sdk_shims(self):
        binary = HOST / "probe"
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DARDUINO",
                  f"-I{HOST / 'include'}", f"-I{FW / 'lib/debug/src'}",
                  f"-I{FW / 'lib/network/src'}",                  f"-I{FW / 'lib/acquisition/src'}", f"-I{FW / 'lib/config/src'}",
                  f"-I{FW / 'lib/telemetry_archive/src'}",
                  f"-I{FW / 'lib/telemetry_contract/src'}",
                  str(HOST / "probe.cpp"), str(FW / "lib/debug/src/debug/http_transport.cpp"),
                  "-o", str(binary)]
        subprocess.run(command, check=True, cwd=ROOT)
        subprocess.run([str(binary)], check=True, cwd=ROOT)

if __name__ == "__main__":
    unittest.main()
