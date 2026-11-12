"""Actual C++ producer regressions, fake SDK/FS; no live backend or HIL.

SIGAP_FIRMWARE_TEST_ROOT optionally selects a frozen source build tree for
before/after verification. ACK fixtures are exact retained host-backend bytes.
"""
from pathlib import Path
import hashlib
import json
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / 'firmware/test/host/debug_app'


class FirmwareArchiveHead(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tree = Path(os.environ.get('SIGAP_FIRMWARE_TEST_ROOT', ROOT))
        if not tree.is_absolute():
            raise ValueError('firmware test tree must be absolute')
        fw = tree / 'firmware'
        cls.temp = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.addClassCleanup(cls.temp.cleanup)
        cls.binary = Path(cls.temp.name) / 'archive-head'
        includes = [HOST / 'include', HOST, fw / 'include', fw / 'src/app']
        includes += sorted((fw / 'lib').glob('*/src'))
        command = ['g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                   '-DARDUINO', '-DSIGAP_DEBUG_APP_HOST_TEST', '-DSIGAP_SENSOR_SIMULATION=1']
        command += ['-Wno-' + warning for warning in (
            'unused-parameter', 'unused-variable', 'unused-but-set-variable',
            'unused-function', 'class-memaccess', 'switch', 'reorder', 'sign-compare')]
        command += [f'-I{path}' for path in includes]
        command += [str(ROOT / 'firmware/test/host/sensor_simulation/archive_head_probe.cpp')]
        command += [str(fw / 'src/app' / name) for name in (
            'sigap_app.cpp', 'runtime_debug_observer.cpp', 'device_control_executor.cpp')]
        command += [str(path) for path in sorted((fw / 'lib').glob('*/src/**/*.cpp'))]
        command += ['-o', str(cls.binary)]
        result = subprocess.run(command, cwd=ROOT, capture_output=True, timeout=180)
        if result.returncode:
            raise RuntimeError(result.stdout.decode() + result.stderr.decode())

    def run_case(self, mode, input_bytes=None):
        return subprocess.run([str(self.binary), mode], cwd=ROOT, input=input_bytes,
                              capture_output=True, timeout=20)

    def assert_passed(self, result):
        self.assertEqual(result.returncode, 0,
                         (result.stdout + result.stderr).decode())

    @staticmethod
    def rows(result, tag):
        prefix = tag.encode() + b' '
        return [line[len(prefix):] for line in result.stdout.splitlines()
                if line.startswith(prefix)]

    def test_first_result_admitted_after_exact_nine_real_ack_prefix(self):
        corpus = json.loads((ROOT / 'tests/fixtures/firmware_archive_head.json').read_text())
        self.assertEqual(len(corpus['pairs']), 9)
        requests = []
        acks = []
        for pair in corpus['pairs']:
            request = pair['request_utf8'].encode()
            ack = pair['ack_utf8'].encode()
            self.assertEqual(hashlib.sha256(request).hexdigest(), pair['request_sha256'])
            self.assertEqual(hashlib.sha256(ack).hexdigest(), pair['ack_sha256'])
            self.assertTrue(ack.endswith(b'\n'))
            requests.append(request)
            acks.append(ack)
        result = self.run_case('head', b''.join(acks))
        self.assertEqual(self.rows(result, 'BATCH'), requests,
                         'retained ACKs must bind byte-identical original request bodies')
        self.assert_passed(result)
        heads = self.rows(result, 'HEAD')
        self.assertEqual(len(heads), 1)
        row = json.loads(heads[0])['results'][0]
        self.assertEqual(row['sequence'], '8')
        self.assertTrue(row['profile_refs'])
        self.assertTrue(row['source_sample_ids'])

    def test_actual_result_serializer_preserves_multiple_profile_refs(self):
        result = self.run_case('profiles')
        self.assert_passed(result)
        row = json.loads(self.rows(result, 'PROFILES')[0])['results'][0]
        self.assertEqual(len(row['profile_refs']), 2)
        self.assertEqual(len(row['source_sample_ids']), 1)

    def test_ds_raw_evidence_and_converted_value_or_explicit_null(self):
        result = self.run_case('ds')
        self.assert_passed(result)
        value = json.loads(self.rows(result, 'DS_VALUE')[0])['samples'][0]
        missing = json.loads(self.rows(result, 'DS_NULL')[0])['samples'][0]
        expected_centi = int(self.rows(result, 'DS_EXPECTED')[0])
        self.assertAlmostEqual(value['converted']['temperature_c'], expected_centi / 100, places=5)
        self.assertIsNone(missing['converted']['temperature_c'])
        self.assertEqual(value['raw'], missing['raw'])
        self.assertTrue(value['raw']['rom'])
        self.assertTrue(value['raw']['scratchpad_hex'])

    def test_extension_noncanonical_flags_rejected_with_valid_frame_crc(self):
        result = self.run_case('flags')
        self.assert_passed(result)
        self.assertIn(b'negative_frames=12', result.stdout)


if __name__ == '__main__':
    unittest.main()
