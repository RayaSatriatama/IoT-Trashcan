"""Host regressions for the Wi-Fi candidate erase-denial trace."""
from __future__ import annotations

import subprocess
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
GUARD = ROOT / "firmware/src/diagnostic_wifi_test_nvs_guards.cpp"
FIXTURE = ROOT / "firmware/test/host/erase_trace"
OBSERVER = ROOT / "firmware/scripts/boot_observe_erase_trace.py"


class EraseTraceTests(unittest.TestCase):
    def test_real_wrappers_deny_all_erase_apis_and_preserve_first_record(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory(prefix="sigap-erase-trace-") as directory:
            executable = Path(directory) / "guard-test"
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread", "-DSIGAP_ERASE_TRACE_HOST_TEST=1",
                f"-I{FIXTURE / 'include'}", f"-I{ROOT / 'firmware/src'}",
                str(GUARD), str(FIXTURE / "test_guard.cpp"), "-o", str(executable),
            ], check=True, cwd=ROOT)
            subprocess.run([str(executable)], check=True, cwd=ROOT)
            subprocess.run([str(executable), "real-counter"], check=True, cwd=ROOT)
            subprocess.run([str(executable), "manager-first"], check=True, cwd=ROOT)
            subprocess.run([str(executable), "ble-first"], check=True, cwd=ROOT)
            subprocess.run([str(executable), "loop-first"], check=True, cwd=ROOT)
            subprocess.run([str(executable), "concurrent"], check=True, cwd=ROOT)
            subprocess.run([str(executable), "paused"], check=True, cwd=ROOT)

    def test_actual_diagnostic_main_fail_closed_paths(self) -> None:
        import tempfile
        with tempfile.TemporaryDirectory(prefix="sigap-main-trace-") as directory:
            executable = Path(directory) / "main-test"
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-DSIGAP_ERASE_TRACE_HOST_TEST=1", f"-I{FIXTURE / 'include'}",
                f"-I{ROOT / 'firmware/src'}", str(GUARD),
                str(FIXTURE / "test_diagnostic_main.cpp"), "-o", str(executable),
            ], check=True, cwd=ROOT)
            for scenario in ("startup", "manager-begin", "entry", "manager-poll", "ble-poll", "paused"):
                subprocess.run([str(executable), scenario], check=True, cwd=ROOT, timeout=5)

    def test_observer_metadata_is_typed_allowlisted_and_precedes_stop(self) -> None:
        import importlib.util
        spec = importlib.util.spec_from_file_location("erase_trace_observer", OBSERVER)
        assert spec and spec.loader
        observer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(observer)
        state = observer.empty()
        observer.consume(b"arbitrary private SDK output", state)
        observer.consume(b"BLE_WIFI_TEST_ERASE primitive=nvs_flash_erase phase=framework_startup pc=0x40381234", state)
        observer.consume(b"BLE_WIFI_TEST_ERASE primitive=nvs_erase_all phase=loop pc=0x403899999", state)
        observer.consume(b"BLE_WIFI_TEST_STOP reason=destructive_erase_blocked", state)
        self.assertEqual(state["erase_denial"], {
            "primitive": "nvs_flash_erase", "phase": "framework_startup", "caller_pc": "0x40381234"
        })
        self.assertEqual(state["stop_reason"], "destructive_erase_blocked")
        self.assertNotIn("private", str(state))

    def test_stop_emits_first_record_and_marks_each_runtime_boundary(self) -> None:
        source = (ROOT / "firmware/src/diagnostic_wifi_test_main.cpp").read_text(encoding="utf-8")
        report = source.split("void report_pending_stop", 1)[1].split("\n}", 1)[0]
        self.assertLess(report.index("emit_erase_trace()"), report.index('Serial.print("BLE_WIFI_TEST_STOP'))
        self.assertIn("if (sigap_ble_wifi_erase_attempted() && !emit_erase_trace()) return;", report)
        self.assertIn("ble_failure_cleanup_done = true;", source)
        self.assertGreaterEqual(source.count("cleanup_ble_failure_once();"), 4)
        for phase in ("manager_begin", "ble_begin", "loop"):
            self.assertIn(f"SigapErasePhase::{phase}", source)
        self.assertIn('stop_with_reason("wifi_manager_start_failed")', source)
        self.assertIn('stop_with_reason("ble_start_failed")', source)


if __name__ == "__main__":
    unittest.main()
