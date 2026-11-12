#!/usr/bin/env python3
"""Run the pinned NimBLE security-store restore path under ASan/UBSan."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

FIXTURES = ("empty", "both", "our-only", "peer-only", "read-error")
SOURCE_RELATIVE = Path(
    "nimble/nimble/host/store/config/src/ble_store_nvs.c"
)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--nimble-root", required=True, type=Path,
        help="NimBLE-Arduino 2.5.1 library directory from .pio/libdeps",
    )
    parser.add_argument("--source", type=Path,
                        help="optional source file override for test fixtures")
    args = parser.parse_args()

    nimble_root = args.nimble_root.resolve()
    source_root = nimble_root / "src"
    production = source_root / SOURCE_RELATIVE
    source = args.source.resolve() if args.source else production
    here = Path(__file__).resolve().parent
    if not source.is_file() or not production.is_file():
        parser.error(f"NimBLE source missing under {nimble_root}")

    with tempfile.TemporaryDirectory(prefix="sigap-nimble-restore-") as temp:
        binary = Path(temp) / "restore"
        command = [
            "gcc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra",
            "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
            "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
            "-DESP_PLATFORM",
            '-DNIMBLE_STORE_SOURCE="' + str(source) + '"',
            "-I" + str(here / "include"), "-I" + str(source_root),
            "-I" + str(production.parent),
            str(here / "restore_harness.c"), "-o", str(binary),
        ]
        compiled = subprocess.run(command, capture_output=True, text=True,
                                 timeout=30)
        print("COMPILE_COMMAND=" + json.dumps(command))
        print("COMPILE_EXIT=" + str(compiled.returncode))
        if compiled.stdout:
            print(compiled.stdout, end="")
        if compiled.stderr:
            print(compiled.stderr, end="", file=sys.stderr)
        if compiled.returncode:
            return compiled.returncode

        env = os.environ.copy()
        env["ASAN_OPTIONS"] = "detect_leaks=0:halt_on_error=1"
        env["UBSAN_OPTIONS"] = "halt_on_error=1:print_stacktrace=1"
        results = []
        for fixture in FIXTURES:
            result = subprocess.run([str(binary), fixture], capture_output=True,
                                    text=True, timeout=10, env=env)
            results.append((fixture, result.returncode, result.stdout,
                            result.stderr))
            item = {"fixture": fixture, "exit_code": result.returncode,
                    "stdout": result.stdout, "stderr": result.stderr}
            print(json.dumps(item))
        passed = sum(code == 0 for _, code, _, _ in results)
        print("SUMMARY=" + json.dumps({
            "source": str(source),
            "source_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
            "production_sha256": hashlib.sha256(production.read_bytes()).hexdigest(),
            "cases": len(results), "passed": passed,
            "failed": len(results) - passed,
        }))
        return 0 if passed == len(FIXTURES) else 1


if __name__ == "__main__":
    raise SystemExit(main())
