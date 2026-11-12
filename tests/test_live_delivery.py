"""Production delivery against finite host FS, not physical HIL."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / 'firmware'
class LiveDelivery(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(dir=os.environ.get('TMPDIR'))
        cls.binary = Path(cls.directory.name) / 'live-delivery'
        includes = [FW/'test/host/reclaim_workspace', FW/'include', *sorted((FW/'lib').glob('*/src'))]
        cmd = ['g++', '-std=c++17', '-Wall', '-Wextra', '-DARDUINO=1',
               *['-I'+str(p) for p in includes],
               str(FW/'test/host/reclaim_workspace/live_delivery.cpp'), '-o', str(cls.binary)]
        subprocess.run(cmd, cwd=ROOT, check=True, timeout=60)
    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()
    def case(self, name):
        subprocess.run([str(self.binary), name], cwd=ROOT, check=True, timeout=30)
    def test_completed_batches_and_ack_journal_are_bounded(self): self.case('bounded')
    def test_restart_at_seven_delivery_boundaries(self): self.case('restart')
    def test_corrupt_tail_is_preserved(self): self.case('corrupt_tail')
    def test_wrong_ack_cannot_reclaim(self): self.case('invalid_ack')
    def test_short_batch_write_is_recovered_before_retry(self): self.case('short_batch')
    def test_short_ack_write_is_recovered_before_retry(self): self.case('short_journal')
    def test_crc_valid_orphan_reclaimed(self): self.case('crc_orphan_reclaimed')
    def test_crc_valid_orphan_acked(self): self.case('crc_orphan_acked')
    def test_crc_valid_pending_without_ack(self): self.case('crc_pending_reclaimed')
    def test_crc_valid_reclaim_before_ack(self): self.case('crc_reclaimed_before_acked')
    def test_crc_valid_duplicate_reclaimed_keeps_legal_prefix(self): self.case('crc_duplicate_reclaimed')
    def test_crc_valid_duplicate_acked_keeps_legal_prefix(self): self.case('crc_duplicate_acked')
    def test_crc_valid_conflicting_hash(self): self.case('crc_conflicting_hash')
    def test_crc_valid_conflicting_count(self): self.case('crc_conflicting_count')
    def test_crc_valid_conflicting_owner(self): self.case('crc_conflicting_owner')
    def test_crc_valid_conflicting_pending(self): self.case('crc_conflicting_pending')
if __name__ == '__main__': unittest.main()
