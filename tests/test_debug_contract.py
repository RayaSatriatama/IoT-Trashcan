import pathlib
import json
import hashlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[1]


class DebugContractTests(unittest.TestCase):
    def test_cpp_probe_matches_actual_producer_fixtures(self):
        with tempfile.TemporaryDirectory(prefix="sigap-debug-contract-") as tmp:
            binary = pathlib.Path(tmp) / "probe"
            subprocess.run([
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                "-Ifirmware/lib/debug/src", "-Ifirmware/lib/acquisition/src", "-Ifirmware/lib/config/src",
                "firmware/test/host/debug_contract/probe.cpp",
                "firmware/lib/debug/src/debug/codec.cpp", "-o", str(binary),
            ], cwd=ROOT, check=True)
            result = subprocess.run([str(binary)], cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr + f"probe exit={result.returncode}")
            records = {}
            for line in result.stdout.splitlines():
                label, sep, value = line.partition("=")
                self.assertTrue(sep, line)
                self.assertNotIn(label, records, f"duplicate label: {label}")
                records[label] = value
            fixture_lines = (ROOT / "contracts/debug/v1/fixtures/ble.txt").read_text().splitlines()
            ble_fixture = dict(line.split("=", 1) for line in fixture_lines)
            self.assertEqual(len(ble_fixture), len(fixture_lines), "duplicate BLE fixture label")
            for label, expected in ble_fixture.items():
                self.assertEqual(records[label], expected)
            observation_path = ROOT / "contracts/debug/v1/fixtures/ble-observation.txt"
            observation = subprocess.run(
                [str(binary), "--ble-observation-fixture"], cwd=ROOT,
                capture_output=True,
            )
            self.assertEqual(observation.returncode, 0, observation.stderr.decode("utf-8", "replace"))
            observation_bytes = observation_path.read_bytes()
            self.assertTrue(observation_bytes.endswith(b"\n"))
            self.assertFalse(observation_bytes.endswith(b"\n\n"))
            self.assertEqual(observation.stdout, observation_bytes)
            observation_records = dict(
                line.split(b"=", 1) for line in observation_bytes.splitlines()
            )
            self.assertEqual(set(observation_records), {
                b"PAGE_10_REQUEST_ZERO", b"PAGE_10_REQUEST_ABSENT",
                b"PAGE_11_OPTIONALS_ZERO", b"PAGE_11_OPTIONALS_ABSENT",
                b"HTTP_BLE_OBSERVATION",
            })
            upload = json.loads(observation_records[b"HTTP_BLE_OBSERVATION"])
            self.assertEqual([(event["module"], event["event_code"], event["arg0"], event["arg1"])
                              for event in upload["events"]],
                             [(2, 10, 0, 3), (2, 10, 1, 3), (2, 11, 3, 0), (2, 11, 3, 1)])
            self.assertEqual(upload["events"][0]["request_id"], 0)
            self.assertIsNone(upload["events"][1]["request_id"])
            self.assertEqual(upload["events"][2]["related_sequence"], "0")
            self.assertIsNone(upload["events"][3]["related_sequence"])
            self.assertEqual(records["EVENT"], (ROOT / "contracts/debug/v1/fixtures/events.json").read_text().strip())
            self.assertEqual(records["HTTP"], (ROOT / "contracts/debug/v1/fixtures/http.json").read_text().strip())
            self.assertEqual(set(records), {"META", "PAGE", "EVENT", "EMPTY_PAGE", "MAX_PAGE", "HTTP", "COUNTERS_MAX", "COUNTERS_ZERO", "HTTP_ESCAPED", "MAX_META", "EVENT_ABSENT", "EVENT_ZERO", "MAX_EVENT", "HTTP_EIGHT", "GAP_PAGE", "PAGE_ZERO", "UPLOAD_BASE_BYTES"})
            for label in ("META", "PAGE", "EMPTY_PAGE", "MAX_PAGE", "MAX_META", "GAP_PAGE", "PAGE_ZERO"):
                self.assertLessEqual(len(records[label].encode("ascii")), 512, label)
            for label in ("HTTP", "HTTP_ESCAPED", "COUNTERS_MAX", "COUNTERS_ZERO"):
                raw = records[label].encode("utf-8")
                self.assertNotIn(b"\0", raw)
                self.assertLessEqual(len(raw), 4096, label)
                obj = json.loads(records[label])
                self.assertIsInstance(obj["overwritten"], str)
                self.assertIsInstance(obj["delivery_lost"], str)
            self.assertLessEqual(len(records["MAX_EVENT"].encode("ascii")), 512)
            self.assertLessEqual(len(records["HTTP_EIGHT"].encode("ascii")), 4096)
            self.assertEqual(len(json.loads(records["HTTP_EIGHT"])["events"]), 8)
            self.assertEqual(json.loads(records["COUNTERS_MAX"])["overwritten"], str(2**64 - 1))
            self.assertEqual(json.loads(records["COUNTERS_ZERO"])["overwritten"], "0")
            event = json.loads(records["EVENT_ZERO"])
            self.assertEqual(event["request_id"], 0)
            self.assertEqual(event["related_sequence"], "0")

            ack_path = ROOT / "contracts/debug/v1/fixtures/ack.json"
            ack = json.loads(ack_path.read_text())
            self.assertEqual(len(ack_path.read_bytes()), 276)
            result = subprocess.run([str(binary), "--ack", str(ack_path)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            fields = result.stdout.strip().removeprefix("ACK=").split("|")
            self.assertEqual(fields, [
                str(ack["schema_version"]), ack["status"],
                str(len(ack["device_id"].encode("utf-8"))),
                ack["device_id"].encode("utf-8").hex(),
                ack["boot_id"], ack["first_sequence"], ack["last_sequence"],
                str(ack["stored_count"]), str(ack["deduplicated_count"]),
                ack["request_sha256"],
            ])

            # Task 1D-C: bind the parsed backend ACK to the independently
            # constructed request snapshot and the exact producer payload.
            http_fixture = (ROOT / "contracts/debug/v1/fixtures/http.json").read_bytes()
            http_wire = records["HTTP"].encode("utf-8")
            self.assertEqual(len(http_fixture), 407)
            self.assertTrue(http_fixture.endswith(b"\n"))
            self.assertEqual(http_fixture[:-1], http_wire)
            self.assertEqual(len(http_wire), 406)
            expected_digest = hashlib.sha256(http_wire).hexdigest()
            self.assertEqual(expected_digest, ack["request_sha256"])

            def matches(raw_ack, label, identity=b"TEST-DEVICE", digest=expected_digest):
                path = pathlib.Path(tmp) / f"match-{label}.json"
                path.write_bytes(raw_ack if isinstance(raw_ack, bytes) else raw_ack.encode("utf-8"))
                result = subprocess.run([
                    str(binary), "--match", str(path), digest, identity.hex(),
                ], cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                return result.stdout.strip()

            self.assertEqual(matches(ack_path.read_bytes(), "actual-go"), "MATCH=1")
            for stored, deduplicated in [(1, 0), (0, 1)]:
                split_ack = dict(ack, stored_count=stored,
                                 deduplicated_count=deduplicated)
                split_raw = json.dumps(split_ack, separators=(",", ":"))
                self.assertEqual(matches(split_raw, f"split-{stored}-{deduplicated}"),
                                 "MATCH=1")
            nul_ack = dict(ack, device_id="A\u0000B")
            nul_raw = json.dumps(nul_ack, separators=(",", ":"))
            self.assertEqual(matches(nul_raw, "nul-identity", identity=b"A\0B"),
                             "MATCH=1")
            self.assertEqual(matches(nul_raw, "nul-identity-tail-mismatch",
                                     identity=b"A\0C"), "MATCH=0")
            for field, value in [
                ("device_id", "TEST-DEVICE-x"),
                ("boot_id", "10000000000000000000000000000001"),
                ("first_sequence", "1"), ("last_sequence", "3"),
                ("stored_count", 2), ("deduplicated_count", 1),
                ("request_sha256", "0" * 64),
            ]:
                altered = dict(ack)
                altered[field] = value
                raw = json.dumps(altered, separators=(",", ":"))
                decoded = pathlib.Path(tmp) / f"decode-{field}.json"
                decoded.write_text(raw)
                parsed = subprocess.run([str(binary), "--ack", str(decoded)],
                                        cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(parsed.returncode, 0, field + parsed.stderr)
                self.assertEqual(matches(raw, field), "MATCH=0", field)
            self.assertEqual(matches(ack_path.read_bytes(), "digest-argument",
                                     digest="00" * 32), "MATCH=0")
            result = subprocess.run([str(binary), "--context-self-test"],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip(),
                             "TEST_CONTEXT=PASS;synthetic=PASS;noncontiguous=2,5;nul+utf8=PASS")

            def rejected(raw, label):
                path = pathlib.Path(tmp) / f"ack-{label}.json"
                path.write_bytes(raw if isinstance(raw, bytes) else raw.encode("utf-8"))
                result = subprocess.run([str(binary), "--ack", str(path)],
                                        cwd=ROOT, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, label)

            def accepted_device(json_string, expected_bytes, label):
                raw = ack_path.read_bytes().replace(b'"TEST-DEVICE"', json_string)
                path = pathlib.Path(tmp) / f"ack-{label}.json"
                path.write_bytes(raw)
                result = subprocess.run([str(binary), "--ack", str(path)],
                                        cwd=ROOT, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, label + result.stdout + result.stderr)
                fields = result.stdout.strip().removeprefix("ACK=").split("|")
                self.assertEqual(fields[2:4], [str(len(expected_bytes)), expected_bytes.hex()], label)

            def changed(**updates):
                value = dict(ack)
                value.update(updates)
                return json.dumps(value, separators=(",", ":"))

            maximum_sequence = "18446744073709551615"
            max_path = pathlib.Path(tmp) / "ack-uint64-max.json"
            max_path.write_text(changed(first_sequence=maximum_sequence,
                                        last_sequence=maximum_sequence,
                                        stored_count=8, deduplicated_count=0))
            result = subprocess.run([str(binary), "--ack", str(max_path)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip().split("|")[5:9],
                             [maximum_sequence, maximum_sequence, "8", "0"])

            rejected(changed(schema_version=2), "schema")
            rejected(changed(schema_version="1"), "schema-string")
            rejected(changed(schema_version=1.0), "schema-float")
            rejected(changed(status="ACCEPTED"), "status")
            rejected(changed(device_id=""), "empty-device")
            rejected(changed(device_id="D" * 65), "long-device")
            accepted_device('"café 🚀"'.encode(), "café 🚀".encode(), "raw-utf8")
            accepted_device(b'"caf\\u00e9 \\ud83d\\ude80"', "café 🚀".encode(), "escaped-unicode")
            accepted_device(b'"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000"',
                            b'"\\/\b\f\n\r\t\0', "escapes-nul")
            accepted_device(b'"\\u03a9\\u20ac"', "Ω€".encode(), "bmp")
            accepted_device(b'"' + b"A" * 64 + b'"', b"A" * 64, "ascii-64")
            accepted_device(b'"' + "é".encode() * 32 + b'"', "é".encode() * 32, "utf8-64")
            rejected(ack_path.read_bytes().replace(b'"TEST-DEVICE"', b'"' + b"A" * 65 + b'"'), "decoded-65")
            rejected(ack_path.read_bytes().replace(b'"TEST-DEVICE"', b'"' + b"\\u0041" * 65 + b'"'), "escaped-decoded-65")
            rejected(ack_path.read_bytes().replace(b'"TEST-DEVICE"', b'"' + "é".encode() * 32 + b'A' + b'"'), "utf8-decoded-65")
            escaped_sequence = ack_path.read_bytes().replace(b'"first_sequence":"2"', b'"first_sequence":"\\u0032"')
            sequence_path = pathlib.Path(tmp) / "ack-escaped-sequence.json"
            sequence_path.write_bytes(escaped_sequence)
            result = subprocess.run([str(binary), "--ack", str(sequence_path)],
                                    cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertEqual(result.stdout.strip().split("|")[5], "2")
            for malformed, label in [
                (b'"\\ud800"', "lone-high"), (b'"\\udc00"', "lone-low"),
                (b'"\\udc00\\ud800"', "reversed-surrogate"),
                (b'"\\ud800\\u0041"', "bad-surrogate-pair"),
                (b'"\\ud800\\u"', "truncated-surrogate-pair"),
                (b'"\\u12"', "truncated-escape"), (b'"\\x20"', "bad-escape"),
                (b'"raw\x01control"', "raw-control"),
                (b'"' + bytes([0xc0, 0xaf]) + b'"', "overlong-utf8"),
                (b'"' + bytes([0x80]) + b'"', "stray-continuation"),
                (b'"' + bytes([0xe2, 0x82]) + b'"', "truncated-utf8"),
                (b'"' + bytes([0xed, 0xa0, 0x80]) + b'"', "surrogate-utf8"),
                (b'"' + bytes([0xf4, 0x90, 0x80, 0x80]) + b'"', "out-of-range-utf8"),
            ]:
                rejected(ack_path.read_bytes().replace(b'"TEST-DEVICE"', malformed), label)
            alias_duplicate = ack_path.read_bytes().replace(
                b'"device_id":"TEST-DEVICE"', b'"device_id":"TEST-DEVICE","device\\u005fid":"TEST-DEVICE"')
            rejected(alias_duplicate, "decoded-duplicate-key")
            rejected(json.dumps({k: v for k, v in ack.items() if k != "status"}), "missing")
            rejected(json.dumps(ack, separators=(",", ":"))[:-1] +
                     ',"status":"DURABLE"}', "duplicate")
            rejected(json.dumps({**ack, "unknown": 1}), "unknown")
            rejected(changed(first_sequence=2), "first-number")
            rejected(changed(last_sequence=2), "last-number")
            rejected(changed(first_sequence="0"), "zero-sequence")
            rejected(changed(first_sequence="02"), "leading-zero")
            rejected(changed(first_sequence="18446744073709551616"), "overflow")
            rejected(changed(first_sequence="3", last_sequence="2"), "reversed")
            rejected(changed(stored_count=9), "stored-overrange")
            rejected(changed(deduplicated_count=9), "dedup-overrange")
            rejected(changed(stored_count=0, deduplicated_count=0), "zero-count")
            rejected(changed(stored_count=8, deduplicated_count=1), "sum-overrange")
            rejected(changed(request_sha256="A" * 64), "digest-case")
            rejected(changed(request_sha256="0" * 63), "digest-length")
            rejected(changed(request_sha256="g" + ack["request_sha256"][1:]), "digest-syntax")
            rejected(changed(boot_id="g" + ack["boot_id"][1:]), "boot-syntax")
            rejected(changed(boot_id="0" * 31), "boot-length")
            rejected(ack_path.read_text()[:-2], "truncated")
            rejected(ack_path.read_text() + " {}", "trailing")
            rejected(ack_path.read_text().rstrip("\n") + " " * 4097, "oversized")


if __name__ == "__main__":
    unittest.main()
