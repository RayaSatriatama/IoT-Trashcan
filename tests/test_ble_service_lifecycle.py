from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BleServiceLifecycleTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workspace = tempfile.TemporaryDirectory(prefix="ble-service-", dir=os.environ["TMPDIR"])
        cls.addClassCleanup(cls.workspace.cleanup)
        cls.binaries = {}
        for variant, flags in (("production", []), ("diagnostic", ["-DSIGAP_BLE_DIAGNOSTIC"]), ("wifi_test", ["-DSIGAP_BLE_WIFI_TEST"])):
            binary = Path(cls.workspace.name) / variant
            command = [
                "c++", "-std=c++17", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
                "-Wno-switch",  # Typed debug/calibration return before the legacy Wi-Fi switch.
                "-DARDUINO", *flags,
                "-Ifirmware/test/host/ble_service/include", "-Ifirmware/lib/provisioning/src",
                *[f"-I{path}" for path in sorted((ROOT / "firmware/lib").glob("*/src"))],
                "firmware/test/host/ble_service/probe.cpp",
                "firmware/lib/provisioning/src/provisioning/ble_provisioning.cpp",
                "firmware/lib/debug/src/debug/commands.cpp", "firmware/lib/debug/src/debug/codec.cpp",
                "firmware/lib/debug/src/debug/service.cpp", "-o", str(binary),
                "firmware/lib/maintenance/src/maintenance/maintenance.cpp",
            ]
            built = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, timeout=90)
            if built.returncode:
                raise AssertionError(f"{variant} compile failed:\n{built.stdout}{built.stderr}")
            cls.binaries[variant] = binary

    def run_case(self, variant, scenario):
        result = subprocess.run([str(self.binaries[variant]), scenario], cwd=ROOT, text=True, capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, f"{variant}/{scenario}: {result.stdout}{result.stderr}")
        self.assertEqual(result.stdout.strip(), f"PASS {scenario}")
        print(f"{variant}/{scenario}: PASS")

    def test_production_callbacks_and_lifecycle(self):
        for scenario in ("deferred", "bounded", "stale", "reused_handle", "delayed_disconnect", "disconnect_during_query", "stale_during_query", "disconnect_between_writes", "stale_hint_between_writes", "stale_callbacks_live", "absent_without_callback", "absent_query_reconnect", "absent_same_handle_reconnect", "overflow_during_query", "positive_mtu_overflow_during_query", "handle_zero_presence", "overflow", "timeout", "long_write", "oversized", "debug_commands"):
            with self.subTest(scenario=scenario):
                self.run_case("production", scenario)

    def test_observed_commands_and_actual_notify_admission_all_variants(self):
        for variant in ("production", "diagnostic", "wifi_test"):
            for scenario in ("observed_commands", "observed_notify_refusal", "observed_filtered", "observed_full", "observed_stale"):
                with self.subTest(variant=variant, scenario=scenario):
                    self.run_case(variant, scenario)

    def test_diagnostic_fault_stays_fail_closed(self):
        self.run_case("diagnostic", "fault")

    def test_wifi_guard_fault_stays_fail_closed(self):
        self.run_case("wifi_test", "fault")


if __name__ == "__main__":
    unittest.main()
