from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SCRATCH = Path.home() / ".hermes" / "cache" / "scratch"


class DebugConcurrencyTest(unittest.TestCase):
    def test_real_mutex_host_probe(self):
        SCRATCH.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="debug-mutex-", dir=SCRATCH) as tmp:
            binary = Path(tmp) / "debug_concurrency_probe"
            command = [
                "c++", "-std=c++17", "-O2", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-Ifirmware/lib/debug/src",
                "-Ifirmware/lib/acquisition/src",
                "-Ifirmware/lib/config/src",
                "firmware/test/host/debug_concurrency/probe.cpp",
                "firmware/lib/debug/src/debug/service.cpp",
                "-o", str(binary),
            ]
            built = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True, check=False)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("mutex_probe PASS produced=6000", result.stdout)
            self.assertIn("overwritten=5936", result.stdout)
            sizes = re.search(r"record_bytes=(\d+) ring_bytes=(\d+) service_bytes=(\d+) snapshot_bytes=(\d+)", result.stdout)
            self.assertIsNotNone(sizes, result.stdout)
            self.assertLessEqual(int(sizes.group(1)), 64)
            self.assertLessEqual(int(sizes.group(2)), 4096)
            print(result.stdout, end="")


if __name__ == "__main__":
    unittest.main()
