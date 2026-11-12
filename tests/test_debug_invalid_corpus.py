import json
import os
import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]
CORPUS = ROOT / "contracts/debug/v1/fixtures/invalid.json"


class DebugInvalidCorpusTests(unittest.TestCase):
    def test_cpp_ack_decoder_rejects_every_shared_ack_case(self):
        corpus = json.loads(CORPUS.read_text(encoding="utf-8"))
        self.assertEqual(corpus["format"], "sigap-debug-invalid-corpus")
        self.assertEqual(corpus["version"], 1)
        self.assertEqual(corpus["encoding"], "raw bytes as lowercase hexadecimal")
        cases = [entry for entry in corpus["cases"] if entry["target"] == "ack"]
        self.assertTrue(cases, "invalid corpus has no ACK cases")
        names = [entry["name"] for entry in corpus["cases"]]
        self.assertEqual(len(names), len(set(names)), "corpus case names must be unique")
        for target in ("ble", "upload", "ack"):
            self.assertTrue(any(entry["target"] == target for entry in corpus["cases"]),
                            f"invalid corpus has no {target} cases")
        for entry in corpus["cases"]:
            self.assertRegex(entry["raw_hex"], r"^(?:[0-9a-f]{2})+$", entry["name"])
        with tempfile.TemporaryDirectory(prefix="sigap-debug-invalid-",
                                         dir=os.environ.get("TMPDIR")) as tmp:
            binary = pathlib.Path(tmp) / "debug-contract-probe"
            compile_result = subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Ifirmware/lib/debug/src", "-Ifirmware/lib/acquisition/src", "-Ifirmware/lib/config/src",
                "firmware/test/host/debug_contract/probe.cpp",
                "firmware/lib/debug/src/debug/codec.cpp", "-o", str(binary),
            ], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            for index, entry in enumerate(cases):
                path = pathlib.Path(tmp) / f"ack-{index}.bin"
                path.write_bytes(bytes.fromhex(entry["raw_hex"]))
                result = subprocess.run([str(binary), "--ack", str(path)],
                                        cwd=ROOT, capture_output=True)
                self.assertNotEqual(result.returncode, 0,
                                    f"{entry['name']}: C++ ACK decoder accepted exact corpus bytes")


if __name__ == "__main__":
    unittest.main()
