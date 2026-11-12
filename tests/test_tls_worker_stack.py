from pathlib import Path
import json
import os
import subprocess
import tempfile
import shutil
import unittest
from datetime import datetime, timezone

ROOT = Path(__file__).resolve().parents[1]
FW = ROOT / "firmware"
HOST = FW / "test/host/tls_worker_stack"


class TlsWorkerStackRegression(unittest.TestCase):
    def test_three_production_transport_translation_units(self):
        evidence_root = Path(os.environ.get("TLS_WORKER_EVIDENCE", tempfile.gettempdir()))
        evidence_root.mkdir(parents=True, exist_ok=True)
        run_id = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%S.%fZ")
        run_dir = tempfile.mkdtemp(prefix=f"tls-worker-{run_id}-", dir=evidence_root)
        evidence = Path(run_dir)
        binary = evidence / "tls_worker_stack_probe"
        host = HOST / "include"
        command = ["g++", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-DARDUINO", "-ffunction-sections", "-fdata-sections",
                   f"-I{host}", f"-I{FW / 'lib/network/src'}", f"-I{FW / 'lib/provisioning/src'}",
                   f"-I{FW / 'lib/debug/src'}", f"-I{FW / 'lib/acquisition/src'}", f"-I{FW / 'lib/config/src'}",
                   f"-I{FW / 'lib/telemetry_archive/src'}", f"-I{FW / 'lib/telemetry_contract/src'}",
                   str(HOST / "probe.cpp"), str(HOST / "sdk_stubs.cpp"),
                   str(FW / "lib/network/src/network/https_transport.cpp"),
                   str(FW / "lib/network/src/network/device_control_http.cpp"),
                   str(FW / "lib/network/src/network/device_control_codec.cpp"),
                   str(FW / "lib/debug/src/debug/http_transport.cpp"),
                   "-Wl,--gc-sections", "-o", str(binary)]
        compile_result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True)
        (evidence / "compile.stdout").write_text(compile_result.stdout)
        (evidence / "compile.stderr").write_text(compile_result.stderr)
        (evidence / "compile.command.json").write_text(json.dumps(command, indent=2) + "\n")
        (evidence / "compile.exit").write_text(f"{compile_result.returncode}\n")
        self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
        run_result = subprocess.run([str(binary)], cwd=ROOT, text=True, capture_output=True)
        (evidence / "run.stdout").write_text(run_result.stdout)
        (evidence / "run.stderr").write_text(run_result.stderr)
        (evidence / "run.command.json").write_text(json.dumps([str(binary)], indent=2) + "\n")
        (evidence / "run.exit").write_text(f"{run_result.returncode}\n")
        self.assertEqual(run_result.returncode, 0, run_result.stdout + run_result.stderr)
        self.assertIn("observed task=sigap_net stack_bytes=16384", run_result.stdout)
        self.assertIn("observed task=sigap_ctl stack_bytes=16384", run_result.stdout)
        self.assertIn("observed task=sigap_dbg_http stack_bytes=16384", run_result.stdout)

if __name__ == "__main__":
    unittest.main(verbosity=2)
