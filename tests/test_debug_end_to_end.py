"""TEST-only actual C++ encoder -> authenticated Go/SQLite -> history proof."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
GO = 'go'


class DebugEndToEndTests(unittest.TestCase):
    def test_actual_producer_and_authenticated_history_are_exact(self):
        with tempfile.TemporaryDirectory(prefix='sigap-task9-codec-') as directory:
            binary = Path(directory) / 'codec'
            subprocess.run([
                'g++', '-std=c++17', '-Wall', '-Wextra', '-Werror',
                '-Ifirmware/lib/debug/src', '-Ifirmware/lib/acquisition/src',
                '-Ifirmware/lib/config/src',
                'firmware/test/host/debug_contract/probe.cpp',
                'firmware/lib/debug/src/debug/codec.cpp', '-o', str(binary),
            ], cwd=ROOT, check=True, timeout=60)
            produced = subprocess.run(
                [str(binary)], cwd=ROOT, check=True, capture_output=True, timeout=30,
            ).stdout
        records = {}
        for line in produced.splitlines():
            label, separator, body = line.partition(b'=')
            self.assertTrue(separator)
            self.assertNotIn(label, records)
            records[label] = body
        raw = records[b'HTTP']
        stored = (ROOT / 'contracts/debug/v1/fixtures/http.json').read_bytes()
        self.assertEqual(stored, raw + b'\n')
        fixture = json.loads((ROOT / 'tests/fixtures/debug-production-history.json').read_bytes())
        upload = json.loads(raw)
        digest = hashlib.sha256(raw).hexdigest()
        self.assertEqual(fixture['provenance'], 'TEST')
        self.assertEqual(fixture['producer_sha256'], digest)
        self.assertEqual(fixture['ack']['request_sha256'], digest)
        self.assertEqual(fixture['ack']['device_id'], upload['device_id'])
        self.assertEqual(fixture['ack']['boot_id'], upload['boot_id'])
        history = fixture['history']
        self.assertEqual(history['device_id'], upload['device_id'])
        self.assertEqual(len(history['events']), len(upload['events']))
        for original, committed in zip(upload['events'], history['events']):
            self.assertEqual({key: committed[key] for key in original}, original)
            self.assertEqual(committed['boot_id'], upload['boot_id'])
            self.assertEqual(committed['build_id'], upload['build_id'])
            self.assertEqual(committed['captured_monotonic_us'], upload['captured_monotonic_us'])
            self.assertNotEqual(committed['received_at'], committed['monotonic_us'])
        self.assertEqual(fixture['expired_operator_status'], 401)
        self.assertEqual(fixture['empty_history']['events'], [])
        self.assertTrue(fixture['retention_history']['retention_gap'])
        self.assertEqual(len({event['boot_id'] for event in fixture['boot_change_history']['events']}), 2)
        # The normal Go test regenerates actual recorder responses in memory and
        # byte-compares this fixture, without updating it or touching a live DB.
        environment = dict(os.environ, GOTOOLCHAIN='local')
        subprocess.run([
            GO, 'test', '-mod=readonly', './internal/http',
            '-run', '^TestTask9', '-count=1',
        ], cwd=ROOT, env=environment, check=True, timeout=120)


if __name__ == '__main__':
    unittest.main()
