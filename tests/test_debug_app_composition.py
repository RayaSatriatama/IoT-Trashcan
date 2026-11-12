from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware"
HOST = FW / "test/host/debug_app"

class DebugAppComposition(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(dir=os.environ.get("TMPDIR"))
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.binaries = {}
        includes = [HOST / "include", FW / "include", FW / "src/app"]
        includes += sorted((FW / "lib").glob("*/src"))
        sources = sorted((FW / "lib").glob("*/src/**/*.cpp"))
        for mode in ("configured", "unconfigured", "debug_only", "no_ca", "full_debug"):
            binary = Path(cls.temporary.name) / mode
            # Suppress only inherited legacy warning classes in unchanged libraries.
            command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Wno-unused-parameter", "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-unused-function", "-Wno-class-memaccess", "-Wno-switch", "-Wno-reorder", "-Wno-sign-compare", "-DARDUINO"]
            command += [f"-I{path}" for path in includes]
            command += ["-DSIGAP_DEBUG_APP_HOST_TEST", "-DSIGAP_USE_SD", "-DSIGAP_SD_TRANSPORT_SDMMC_1BIT"]
            if mode == "full_debug":
                command += ["-DSIGAP_FULL_DEBUG_BUILD"]
            if mode != "debug_only":
                command += ['-DSIGAP_NETWORK_ENDPOINT="https://example.invalid"']
            if mode != "no_ca":
                command += ['-DSIGAP_NETWORK_CA_CERT="TEST-CA"']
            if mode != "unconfigured":
                command += ['-DSIGAP_DEBUG_ENDPOINT="https://example.invalid/api/device-debug/device/TEST-DEVICE/events"', '-DSIGAP_DEBUG_DEVICE_CODE="TEST-DEVICE"']
            command += [str(HOST / "probe.cpp"), str(FW / "src/app/sigap_app.cpp"), str(FW / "src/app/runtime_debug_observer.cpp")]
            command += [str(FW / "src/app/device_control_executor.cpp")]
            command += [str(path) for path in sources]
            command += ["-o", str(binary)]
            subprocess.run(command, check=True, cwd=ROOT)
            cls.binaries[mode] = binary

    def run_case(self, scenario, mode="configured", capture=False):
        result = subprocess.run([str(self.binaries[mode]), scenario], check=True, cwd=ROOT,
                                stdout=subprocess.PIPE if capture else None, text=capture)
        if capture:
            print(result.stdout, end="")
            return result.stdout

    def test_full_app_usb_ble_routes_share_one_debug_service(self):
        output = self.run_case("composition_routes", capture=True)
        self.assertIsNotNone(output)
        self.assertIn("COMPOSITION_ROUTES PASS", output)

    def test_model53_actual_lifecycle_transitions_all_variants(self):
        for mode in ("configured", "unconfigured", "debug_only", "no_ca", "full_debug"):
            with self.subTest(mode=mode):
                output = self.run_case("model53_transitions", mode, capture=True)
                assert output is not None
                self.assertIn("MODEL53_TRANSITIONS PASS keys=4 events=16", output)
                lines = [line for line in output.splitlines() if line.startswith("MODEL53 key=")]
                self.assertEqual(len(lines), 16)
                for key in range(4):
                    self.assertIn(f"MODEL53 key={key} state=2 version={7 + key}", output)
                    self.assertIn(f"MODEL53 key={key} state=0 version=4294967295 observed=2147483647", output)

    def test_full_app_info_and_debug_build_defaults_are_volatile(self):
        info = self.run_case("default_level", capture=True)
        debug = self.run_case("default_level", "full_debug", capture=True)
        self.assertIsNotNone(info)
        self.assertIsNotNone(debug)
        self.assertIn("DEFAULT_LEVEL INFO PASS", info)
        self.assertIn("DEFAULT_LEVEL DEBUG PASS", debug)

    def test_actual_archive_control_priority(self):
        self.run_case("priority")

    def test_active_calibration_defers_debug_admission(self):
        self.run_case("calibration_priority")

    def test_configured_debug_does_not_need_telemetry_endpoint(self):
        self.run_case("priority", "debug_only")

    def test_actual_response_and_tls_cleanup_exclusion(self):
        self.run_case("response")

    def test_cancel_before_task_cleanup_retains_io(self):
        self.run_case("cancel")

    def test_controller_expiry_and_acquisition_continue_during_debug_cleanup(self):
        self.run_case("expiry")

    def test_missing_debug_configuration(self):
        self.run_case("unavailable", "unconfigured")

    def test_missing_ca(self):
        self.run_case("unavailable", "no_ca")

    def test_missing_credential(self):
        self.run_case("credential")

    def test_control_cleanup_progresses_after_time_readiness_loss(self):
        self.run_case("control_time_loss")

    def test_wifi_loss_cancels_debug_without_early_release(self):
        self.run_case("wifi_loss")

    def test_missing_time(self):
        self.run_case("time")

    def test_missing_wifi(self):
        self.run_case("wifi")

    def test_debug_without_telemetry_endpoint_preserves_time_gate(self):
        self.run_case("time", "debug_only")

    def test_observer_forwards_real_app_evidence_and_event(self):
        self.run_case("positive")

    def test_runtime_startup_positive_all_variants(self):
        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                self.run_case("startup_positive", mode)

    def test_runtime_startup_missing_config_all_variants(self):
        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                self.run_case("startup_config_failure", mode)

    def test_runtime_startup_filter_and_full_ring(self):
        def effects(scenario, mode):
            output = self.run_case(scenario, mode, capture=True)
            assert output is not None
            lines = [line for line in output.splitlines() if line.startswith("STARTUP_EFFECTS ")]
            self.assertEqual(len(lines), 1)
            return dict(field.split("=", 1) for field in lines[0].split()[1:])

        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                baseline = effects("startup_positive", mode)
                self.assertEqual(effects("startup_filter", mode), baseline)
                self.assertEqual(effects("startup_full", mode), baseline)

    def test_observer_filter_does_not_suppress_cache(self):
        self.run_case("filter")

    def test_observer_full_ring_does_not_suppress_cache(self):
        self.run_case("full")

    def test_observer_coalescing_preserves_distinct_evidence(self):
        self.run_case("coalesced")

    def test_observer_does_not_allocate_or_consume_handoffs(self):
        self.run_case("ownership")

    def test_routine_debug_reads_do_not_create_events(self):
        self.run_case("routine")

    def test_actual_storage_archive_ack_reclaim_service_chain(self):
        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                self.run_case("storage_chain", mode)

    def test_storage_chain_keeps_business_success_when_events_filtered(self):
        self.run_case("storage_chain_filtered")

    def test_storage_chain_overwrites_and_accepts_at_full_ring(self):
        self.run_case("storage_chain_full")

    def test_wifi_scan_event_uses_real_app_debug_service(self):
        self.run_case("wifi_scan")

    def test_actual_acquisition_loop_uses_bound_observer(self):
        self.run_case("acquisition_loop")

    def test_actual_acquisition_and_processing_use_one_debug_service(self):
        self.run_case("processing_chain")

    def test_observed_loop_gap_boundaries_and_saturation_all_variants(self):
        cases = ("loopgap_healthy", "loopgap_late", "loopgap_late_max",
                 "loopgap_stalled", "loopgap_int32max", "loopgap_saturated", "loopgap_recovery",
                 "loopgap_filter", "loopgap_passive",
                 "loopgap_regression")
        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            for scenario in cases:
                with self.subTest(mode=mode, scenario=scenario):
                    self.run_case(scenario, mode)

    def test_loop_gap_business_progress_and_nonempty_ownership_all_variants(self):
        def effects(scenario, mode):
            output = self.run_case(scenario, mode, capture=True)
            assert output is not None
            lines = [line for line in output.splitlines() if line.startswith("LOOPGAP_EFFECTS ")]
            self.assertEqual(len(lines), 1)
            return dict(field.split("=", 1) for field in lines[0].split()[1:])

        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                baseline = effects("loopgap_business_debug", mode)
                self.assertEqual(effects("loopgap_business_filtered", mode), baseline)
                self.assertEqual(effects("loopgap_full", mode), baseline)

    def test_ble_producer_binding_and_business_ownership_all_variants(self):
        def effects(scenario, mode):
            output = self.run_case(scenario, mode, capture=True)
            assert output is not None
            lines = [line for line in output.splitlines() if line.startswith("BLE_EFFECTS ")]
            self.assertEqual(len(lines), 1)
            return dict(field.split("=", 1) for field in lines[0].split()[1:])

        for mode in ("configured", "unconfigured", "debug_only", "no_ca"):
            with self.subTest(mode=mode):
                baseline = effects("ble_chain", mode)
                self.assertEqual(effects("ble_chain_filtered", mode), baseline)
                self.assertEqual(effects("ble_chain_full", mode), baseline)

    def test_processing_chain_preserves_business_when_events_filtered(self):
        self.run_case("processing_chain_filtered")

    def test_processing_chain_accepts_events_in_full_real_ring(self):
        self.run_case("processing_chain_full")

if __name__ == "__main__":
    unittest.main()
