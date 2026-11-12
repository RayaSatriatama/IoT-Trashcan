"""Production reclaim admission against host-only finite media and fault injection."""
import os
import shlex
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware"
HOST = FW / "test/host/reclaim_workspace"


def compile_probe(command, timeout):
    print("HOST_COMPILE", shlex.join(command), flush=True)
    result = subprocess.run(command, cwd=ROOT, capture_output=True, text=True, timeout=timeout)
    print(result.stdout, end="")
    print(result.stderr, end="")
    print("HOST_COMPILE_EXIT", result.returncode, flush=True)
    result.check_returncode()


class ReclaimWorkspace(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "reclaim-workspace"
        includes = [HOST, FW / "include", *sorted((FW / "lib").glob("*/src"))]
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-DARDUINO=1"]
        command += [f"-I{path}" for path in includes]
        command += [str(HOST / "reclaim.cpp"), "-o", str(cls.binary)]
        compile_probe(command, 120)

    def run_case(self, scenario):
        result = subprocess.run([str(self.binary), scenario], cwd=ROOT,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"CASE {scenario} PASS", result.stdout)
        print(result.stdout, end="")

    def test_exact_threshold_preserves_unrelated_bytes_and_order(self):
        self.run_case("exact_threshold")

    def test_absent_probe(self):
        self.run_case("absent_probe")

    def test_failed_probe_ignores_ample_output(self):
        self.run_case("failed_probe")

    def test_zero_free(self):
        self.run_case("zero_free")

    def test_one_byte_below_threshold(self):
        self.run_case("below_threshold")

    def test_repeated_denial_then_success(self):
        self.run_case("repeated_then_success")

    def test_wrong_identity_does_not_consult_capacity(self):
        self.run_case("wrong_identity")

    def test_wrong_hash_does_not_consult_capacity(self):
        self.run_case("wrong_hash")

    def test_preexisting_auxiliary_files_are_not_mutated(self):
        self.run_case("preserve_auxiliary")

    def test_admitted_short_write_then_retry(self):
        self.run_case("short_write_retry")

    def test_admitted_rename_failure_then_retry(self):
        self.run_case("rename_failure_retry")

    def test_interrupted_replacement_does_not_resurrect_acked_record(self):
        self.run_case("recovery_committed_replacement")

    def test_uint64_overflow_host_fault_injection(self):
        self.run_case("overflow_host_injection")

    def test_invalid_source_size_host_fault_injection(self):
        self.run_case("source_size_underflow")

    def test_finite_media_and_shared_handle_instrumentation_control(self):
        self.run_case("finite_media_and_alias_control")

    def test_actual_legacy_ack_retries_without_archive_loss(self):
        self.run_case("legacy_ack_low_space_retry")


class ApplicationCapacity(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binary = Path(cls.temporary.name) / "app-capacity"
        app_host = FW / "test/host/debug_app"
        includes = [app_host / "include", app_host, FW / "include", FW / "src/app"]
        includes += sorted((FW / "lib").glob("*/src"))
        command = ["g++", "-std=c++17", "-DARDUINO", "-DSIGAP_DEBUG_APP_HOST_TEST",
                   "-DSIGAP_SENSOR_SIMULATION=1",
                   '-DSIGAP_NETWORK_ENDPOINT="https://host-test.invalid"',
                   '-DSIGAP_NETWORK_CA_CERT="HOST_TEST_CA"']
        command += [f"-I{path}" for path in includes]
        command += [str(HOST / "app_capacity.cpp")]
        command += [str(FW / "src/app" / name) for name in
                    ("sigap_app.cpp", "runtime_debug_observer.cpp", "device_control_executor.cpp")]
        command += [str(path) for path in sorted((FW / "lib").glob("*/src/**/*.cpp"))]
        command += ["-o", str(cls.binary)]
        compile_probe(command, 180)

    def run_case(self, scenario):
        result = subprocess.run([str(self.binary), scenario], cwd=ROOT,
                                capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn(f"APP_CAPACITY {scenario} PASS", result.stdout)
        print(result.stdout, end="")

    def test_valid_geometry(self):
        self.run_case("valid_geometry")

    def test_sdk_failure(self):
        self.run_case("sdk_error")

    def test_zero_total(self):
        self.run_case("zero_total")

    def test_used_exceeds_total(self):
        self.run_case("used_exceeds_total")

    def test_zero_free(self):
        self.run_case("zero_free")

    def test_below_threshold(self):
        self.run_case("below_threshold")

    def test_exact_threshold(self):
        self.run_case("exact_threshold")

    def test_retry_after_sdk_failure(self):
        self.run_case("retry_after_sdk_error")


if __name__ == "__main__":
    unittest.main()
