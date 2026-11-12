"""Synthetic old archive layouts through actual codec/builder/store and ACK runtime."""
from pathlib import Path
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'


class LegacyPendingSelection(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'legacy-pending'
        includes = [FW / 'test/native/test_arduino_storage', FW / 'include']
        includes += sorted((FW / 'lib').glob('*/src'))
        command = ['g++', '-std=c++17', '-Wall', '-Wextra']
        command += [f'-I{p}' for p in includes]
        command += [str(FW / 'test/host/sensor_simulation/legacy_probe.cpp'),
                    '-o', str(cls.binary)]
        subprocess.run(command, cwd=ROOT, check=True, timeout=120)

    def run_scenario(self, scenario):
        subprocess.run([str(self.binary), scenario], cwd=ROOT, check=True, timeout=15)

    def test_retained_original_survives_exact_ack_of_eligible_record(self):
        self.run_scenario('retain_ack')

    def test_current_boot_fault_is_not_hidden_as_legacy(self):
        self.run_scenario('current_fault')

    def test_bounded_scanning_repeat_and_restart(self):
        self.run_scenario('bounded_restart')

    def test_io_faults_and_retained_bytes_still_consume_quota(self):
        self.run_scenario('io_quota')

    def test_original_sht_ticks_are_not_reconstructed(self):
        self.run_scenario('sht_ticks')

    def test_valid_ack_after_restart_before_any_peek(self):
        self.run_scenario('ack_restart')

    def test_null_archive_blocks_ack_retirement_until_rebound(self):
        self.run_scenario('null_archive')

    def test_recreated_runtime_without_archive_blocks_ack_retirement(self):
        self.run_scenario('null_archive_restart')

    def test_preexisting_unsupported_manifest_bytes_remain_unchanged(self):
        self.run_scenario('cached_manifest')

    def test_foreign_device_and_invalid_boot_are_not_hidden(self):
        self.run_scenario('identity_guards')

    def test_canonical_foreign_and_zero_identity_fault_before_eligibility(self):
        self.run_scenario('canonical_identity')

    def test_retained_eligible_interleaving_append_ack_restart(self):
        self.run_scenario('interleaving')

    def test_historical_malformed_result_retains_original_frame(self):
        self.run_scenario('historical_result')

    def test_fixture_bytes_match_documented_pre_extension_layout(self):
        expected = json.loads((ROOT / 'tests/fixtures/firmware_legacy_pending.json').read_text())
        actual = subprocess.run([str(self.binary), 'emit_fixtures'], cwd=ROOT,
                                check=True, capture_output=True, text=True, timeout=15)
        self.assertEqual(json.loads(actual.stdout), expected)


if __name__ == '__main__':
    unittest.main()
