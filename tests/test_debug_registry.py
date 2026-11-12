import json
import pathlib
import re
import subprocess
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
REGISTRY = ROOT / "contracts/debug/v1/registry.json"
PROBE = ROOT / "firmware/test/host/debug_registry/probe.cpp"


class DebugRegistryParityTest(unittest.TestCase):
    def test_compiled_registry_matches_json_and_has_closed_argument_meanings(self):
        with tempfile.TemporaryDirectory(prefix="debug-registry-") as temp_dir:
            executable = pathlib.Path(temp_dir) / "debug_registry_probe"
            subprocess.run(
                [
                    "g++",
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    f"-I{ROOT / 'firmware/lib/debug/src'}",
                    f"-I{ROOT / 'firmware/lib/acquisition/src'}",
                    f"-I{ROOT / 'firmware/lib/config/src'}",
                    str(PROBE),
                    "-o",
                    str(executable),
                ],
                check=True,
                cwd=ROOT,
                capture_output=True,
                text=True,
            )
            output = subprocess.run(
                [str(executable)], check=True, capture_output=True, text=True, cwd=ROOT
            ).stdout

        compiled = []
        for line in output.splitlines():
            module, code, is_registered, name, arg0, arg1 = line.split("\t")
            self.assertEqual(is_registered, "1", f"unregistered compiled event {module}:{code}")
            compiled.append(
                {
                    "module": int(module),
                    "code": int(code),
                    "name": name,
                    "arg0": arg0,
                    "arg1": arg1,
                }
            )

        registry = json.loads(REGISTRY.read_text(encoding="utf-8"))
        declared = registry["events"]
        identity = lambda event: (event["module"], event["code"])
        self.assertEqual(len({identity(event) for event in declared}), len(declared))
        self.assertEqual(len({identity(event) for event in compiled}), len(compiled))
        self.assertEqual(compiled, declared)
        wire_rows = re.findall(
            r"^\| .*?\((\d+)\) \| (\d+): (\w+) \|",
            (ROOT / "contracts/debug/v1/wire.md").read_text(encoding="utf-8"),
            flags=re.MULTILINE,
        )
        self.assertEqual(
            [(int(module), int(code), name) for module, code, name in wire_rows],
            [(event["module"], event["code"], event["name"]) for event in declared],
        )

        ble = {event["code"]: event for event in declared if event["module"] == 2}
        self.assertEqual(ble[10]["arg1"], "reason:0=normal,1=timeout,2=error,3=unknown")
        self.assertEqual(ble[11]["arg0"], "reason:0=normal,1=timeout,2=error,3=unknown")
        pending = "pending_request_count; unit=session-owned admitted async logical requests awaiting backend completion; range=0..2147483647; saturated; zero=no pending requests"
        self.assertEqual(ble[11]["arg1"], pending)
        self.assertEqual(ble[16]["arg1"], pending)
        progress = "chunks_sent; unit=SDK notify-call admitted response chunks; range=0..2147483647; saturated; zero=no chunks admitted"
        self.assertEqual(ble[13]["arg0"], progress)
        for event in declared:
            for argument in ("arg0", "arg1"):
                meaning = event[argument]
                self.assertTrue(meaning.strip(), f"empty {argument}: {identity(event)}")
                self.assertNotRegex(
                    meaning,
                    r"(?:^|_)(?:error_class|sensor_enum|reason_enum|profile_kind|stage_index|http_class)$",
                    f"unresolved {argument}: {identity(event)}",
                )
                self.assertNotRegex(
                    meaning.lower(),
                    r"\b(?:raw|pointer|free[- ]form|arbitrary|native ordinal)\b",
                    f"unsafe {argument}: {identity(event)}",
                )
                if re.match(r"(?:count(?:;|$)|count_delta|depth|capacity|elapsed|duration|remaining|attempt|request_bytes|response_bytes|chunks_|network_count|record_count|pending_request_count|retry_after|oldest_age|model_version)", meaning):
                    self.assertRegex(
                        meaning,
                        r"; unit=[^;]+; range=0\.\.2147483647; saturated; zero=",
                        f"incomplete count/unit policy for {argument}: {identity(event)} ({meaning})",
                    )


if __name__ == "__main__":
    unittest.main()
