"""Cross-translation-unit regression for diagnostic DHCP cache suppression."""
from __future__ import annotations

import subprocess
import tempfile
from pathlib import Path
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIXTURE = ROOT / "firmware/test/host/dhcp_cache"
CACHE_MODULE = ROOT / "firmware/src/diagnostic_wifi_test_dhcp_cache.cpp"
WRAPS = [
    "-Wl,--wrap=dhcp_ip_addr_restore",
    "-Wl,--wrap=dhcp_ip_addr_store",
    "-Wl,--wrap=dhcp_ip_addr_erase",
]


class DhcpCacheSuppressionTests(unittest.TestCase):
    def test_linked_sdk_cache_helpers_are_suppressed_for_diagnostic(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sigap-dhcp-cache-") as directory:
            positive = Path(directory) / "positive"
            common = [
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{FIXTURE / 'include'}", str(FIXTURE / "sdk_probe.cpp"),
                str(FIXTURE / "caller.cpp"),
            ]
            subprocess.run([*common, "-o", str(positive)], check=True, cwd=ROOT)
            subprocess.run([str(positive), "positive"], check=True, cwd=ROOT)

            candidate = Path(directory) / "candidate"
            subprocess.run([
                *common, "-DSIGAP_BLE_WIFI_TEST=1", str(CACHE_MODULE),
                *WRAPS, "-o", str(candidate),
            ], check=True, cwd=ROOT)
            subprocess.run([str(candidate)], check=True, cwd=ROOT)


if __name__ == "__main__":
    unittest.main()
