from __future__ import annotations

import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
FIRMWARE = ROOT / "firmware"


class BleNoPasskeySourceContractTest(unittest.TestCase):
    def test_network_diagnostics_use_deep_library_discovery_only_on_diagnostic_targets(self) -> None:
        config = (FIRMWARE / "platformio.ini").read_text(encoding="utf-8")
        for env in ("sigap_ble_diag", "sigap_ble_wifi_test"):
            with self.subTest(environment=env):
                section = config.split(f"[env:{env}]", 1)[1].split("\n[env:", 1)[0]
                self.assertIn("lib_ldf_mode = deep+", section)
        production = config.split("[env:sigap_n16r8]", 1)[1].split("\n[env:", 1)[0]
        native = config.split("[env:native]", 1)[1]
        self.assertNotIn("lib_ldf_mode = deep+", production)
        self.assertNotIn("lib_ldf_mode = deep+", native)

    def test_active_ble_and_usb_sources_have_no_secret_or_authentication_path(self) -> None:
        files = (
            FIRMWARE / "lib/provisioning/src/provisioning/ble_provisioning.h",
            FIRMWARE / "lib/provisioning/src/provisioning/ble_provisioning.cpp",
            FIRMWARE / "lib/maintenance/src/maintenance/maintenance.h",
            FIRMWARE / "lib/maintenance/src/maintenance/maintenance.cpp",
            FIRMWARE / "lib/maintenance/src/maintenance/runtime.h",
            FIRMWARE / "lib/maintenance/src/maintenance/runtime.cpp",
            FIRMWARE / "lib/maintenance/src/maintenance/ble_calibration_backend.h",
            FIRMWARE / "lib/maintenance/src/maintenance/ble_calibration_backend.cpp",
            FIRMWARE / "lib/calibration/src/calibration/calibration_transport.h",
            FIRMWARE / "src/diagnostic_ble_main.cpp",
        )
        source = "\n".join(path.read_text(encoding="utf-8") for path in files)
        forbidden = (
            "NvsProvisioningSecretStore", "provisioning_secret_store",
            "setup_passkey", "LocalPasskeyHandoff", "ILocalPasskeyHandoff",
            "on_authenticated", "onAuthenticationComplete", "AUTH_REQUIRED",
            "READ_AUTHEN", "WRITE_AUTHEN", "setSecurityPasskey",
            "startSecurity", "ble passkey", "BLE_PASSKEY",
            "BLE authentication is established by GATT",
            "authenticated BLE session", "Stable authenticated principal identity.",
        )
        for symbol in forbidden:
            with self.subTest(symbol=symbol):
                self.assertNotIn(symbol, source)
        self.assertIn("NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_NR", source)
        self.assertIn("NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY", source)
        self.assertIn("NimBLEDevice::setSecurityAuth(false, false, true)", source)
        self.assertIn("NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT)", source)

    def test_ble_only_secret_store_files_are_removed_and_app_credentials_remain(self) -> None:
        for name in ("provisioning_secret_store.h", "provisioning_secret_store.cpp"):
            with self.subTest(name=name):
                self.assertFalse(
                    (FIRMWARE / "lib/provisioning/src/provisioning" / name).exists()
                )

        app_header = (FIRMWARE / "src/app/sigap_app.h").read_text(encoding="utf-8")
        app_source = (FIRMWARE / "src/app/sigap_app.cpp").read_text(encoding="utf-8")
        network = (FIRMWARE / "lib/network/src/network/device_control_http.cpp").read_text(encoding="utf-8")
        self.assertIn("NvsConfigStore", app_header)
        self.assertIn("NvsSecretStore", app_header)
        self.assertIn("NvsProfileStore", app_header)
        self.assertIn("set_calibration_transport_controller(", app_source)
        self.assertIn("client.setCACert(ca_certificate_)", network)
        self.assertIn('http.addHeader("Authorization", authorization.data())', network)

    def test_bond_persistence_is_compile_time_disabled_and_effectively_asserted(self) -> None:
        flags = (FIRMWARE / "platformio.ini").read_text(encoding="utf-8")
        implementation = (FIRMWARE / "lib/provisioning/src/provisioning/ble_provisioning.cpp").read_text(encoding="utf-8")
        self.assertIn("-DCONFIG_BT_NIMBLE_MAX_BONDS=0", flags)
        self.assertIn("-DMYNEWT_VAL_BLE_STORE_CONFIG_PERSIST=0", flags)
        self.assertIn("static_assert(MYNEWT_VAL(BLE_STORE_CONFIG_PERSIST) == 0", implementation)
        self.assertIn("static_assert(MYNEWT_VAL(BLE_STORE_MAX_BONDS) == 0", implementation)

    def test_non_ble_credentials_and_configuration_composition_remain(self) -> None:
        app_header = (FIRMWARE / "src/app/sigap_app.h").read_text(encoding="utf-8")
        app_source = (FIRMWARE / "src/app/sigap_app.cpp").read_text(encoding="utf-8")
        maintenance = (FIRMWARE / "lib/maintenance/src/maintenance/runtime.h").read_text(encoding="utf-8")
        self.assertIn("NvsConfigStore", app_header)
        self.assertIn("NvsSecretStore", app_header)
        self.assertIn("NvsProfileStore", app_header)
        self.assertIn("set_calibration_transport_controller(", app_source)
        self.assertIn("provisioning::SecretStore& secrets", maintenance)

    def test_wifi_settings_labels_matching_device_identity_without_claiming_authentication(self) -> None:
        ui = (ROOT / "components/device/WifiSettingsDialog.tsx").read_text(encoding="utf-8")
        device_page = (ROOT / "app/devices/[id].tsx").read_text(encoding="utf-8")
        self.assertIn("identityMatches ? 'Identitas cocok' : 'Belum dicocokkan'", ui)
        self.assertIn("BLE memakai pencocokan identitas tanpa autentikasi", ui)
        self.assertIn("otorisasi API terpisah", ui)
        self.assertNotIn("terautentikasi", ui.lower())
        self.assertIn("<WifiSettingsDialog", device_page)
        self.assertIn("Kalibrasi melalui BLE", device_page)
        self.assertIn("Kalibrasi melalui HTTPS", device_page)
        self.assertNotIn("terautentikasi", device_page.lower())

    def test_manual_target_name_matches_diagnostic_advertisement_rule(self) -> None:
        html = (ROOT / "scripts/ble-manual/index.html").read_text(encoding="utf-8")
        session = (ROOT / "scripts/ble-manual/session.ts").read_text(encoding="utf-8")
        manual = (ROOT / "docs/task9-ble-manual-testing.md").read_text(encoding="utf-8")
        report = " ".join((ROOT / "docs/task9-ble-no-passkey-verification.md").read_text(encoding="utf-8").split())
        firmware = (FIRMWARE / "src/diagnostic_ble_main.cpp").read_text(encoding="utf-8")
        target = re.search(r"<dt>Target wire ID</dt><dd>([0-9a-f]{16})</dd>", html)
        session_target = re.search(r"EXPECTED_DEVICE_ID = '([0-9a-f]{16})'", session)
        instruction = re.search(r"Pilih <code>(SIGAP-[A-Z0-9]+)</code>", html)
        self.assertIsNotNone(target)
        self.assertIsNotNone(session_target)
        self.assertIsNotNone(instruction)
        self.assertIn('"SIGAP-%04llX"', firmware)
        self.assertIn("device_mac & 0xFFFFU", firmware)
        # Recorded hardware MAC fixture, not a new physical readback. Arduino
        # exposes the eFuse bytes as a little-endian uint64 on this target.
        mac_bytes = bytes.fromhex("98a316f5c150")
        sdk_mac_value = int.from_bytes(mac_bytes + bytes(2), byteorder="little")
        expected_wire_id = f"{sdk_mac_value:016x}"
        expected_name = f"SIGAP-{sdk_mac_value & 0xFFFF:04X}"
        self.assertEqual(target.group(1), expected_wire_id)
        self.assertEqual(session_target.group(1), expected_wire_id)
        self.assertEqual(instruction.group(1), expected_name)
        self.assertIn(expected_wire_id, manual)
        self.assertIn(expected_name, manual)
        self.assertIn("bukan pembacaan fisik baru", manual)
        self.assertIn(expected_wire_id, report)
        self.assertIn(expected_name, report)
        self.assertIn("not a new physical readback", report)

    def test_web_calibration_runbook_separates_operator_auth_from_prototype_ble(self) -> None:
        runbook = " ".join((ROOT / "docs/task9-web-calibration.md").read_text(encoding="utf-8").split())
        self.assertIn("The operator API requires an authenticated operator session", runbook)
        self.assertIn("The owner-approved prototype BLE link uses unauthenticated GATT", runbook)
        self.assertIn("not authentication or proof of possession", runbook)
        self.assertIn("DEFERRED_BY_OWNER", runbook)
        self.assertIn("not a prerequisite for current prototype use", runbook)

    def test_active_manual_instructions_do_not_reinstate_ble_pairing_prerequisites(self) -> None:
        paths = (
            ROOT / "README.md",
            ROOT / "docs/TASK9_Customization.md",
            ROOT / "docs/task9-ble-manual-testing.md",
            ROOT / "scripts/ble-manual/index.html",
        )
        active_text = "\n".join(path.read_text(encoding="utf-8") for path in paths).lower()
        forbidden_phrases = (
            "ble passkey", "passkey handoff", "one-shot usb passkey",
            "authenticated mitm gatt", "mitm protection",
        )
        for phrase in forbidden_phrases:
            with self.subTest(phrase=phrase):
                self.assertNotIn(phrase, active_text)


class CandidateWifiFailClosedSourceTest(unittest.TestCase):
    def test_late_erase_latch_denies_station_poll_and_provisioning_callbacks(self) -> None:
        main = (FIRMWARE / "src/diagnostic_wifi_test_main.cpp").read_text(encoding="utf-8")
        service = (FIRMWARE / "lib/provisioning/src/provisioning/ble_provisioning.cpp").read_text(encoding="utf-8")
        guard = (FIRMWARE / "src/diagnostic_wifi_test_nvs_guards.cpp").read_text(encoding="utf-8")
        flags = (FIRMWARE / "platformio.ini").read_text(encoding="utf-8")
        loop = main.split("void loop() {", 1)[1]
        self.assertLess(loop.index("sigap_ble_wifi_erase_attempted()"), loop.index("wifi_manager.poll()"))
        self.assertIn("if (sigap_ble_wifi_erase_attempted()) return;", service)
        self.assertIn("if (sigap_ble_wifi_erase_attempted()) {", service)
        for operation in ("nvs_erase_key", "nvs_erase_all"):
            with self.subTest(operation=operation):
                self.assertIn(f"__wrap_{operation}", guard)
                self.assertIn(f"-Wl,--wrap={operation}", flags)
                start = guard.index(f"__wrap_{operation}")
                body = guard[start:guard.index("\n}", start) + 2]
                self.assertIn(f"SIGAP_DENY({operation});", body)
                self.assertIn("ESP_ERR_INVALID_STATE", body)


class NvsGuardWrappersTest(unittest.TestCase):
    def test_record_access_and_all_erase_paths_fail_closed_without_calling_flash(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sigap-nvs-guards-", dir=os.environ["TMPDIR"]) as tmp:
            work = Path(tmp)
            include = work / "include"
            include.mkdir()
            (include / "esp_err.h").write_text(
                "#pragma once\nusing esp_err_t = int;\n"
                "constexpr esp_err_t ESP_OK = 0;\n"
                "constexpr esp_err_t ESP_FAIL = 5;\n"
                "constexpr esp_err_t ESP_ERR_INVALID_STATE = 6;\n",
                encoding="utf-8",
            )
            (include / "esp_partition.h").write_text(
                """#pragma once
#include <cstddef>
#include "esp_err.h"
enum { ESP_PARTITION_TYPE_DATA = 1, ESP_PARTITION_SUBTYPE_DATA_NVS = 2 };
struct esp_partition_t { int type; int subtype; char label[17]; std::size_t size; };
esp_err_t esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t);
""",
                encoding="utf-8",
            )
            (include / "nvs.h").write_text(
                """#pragma once
#include <cstddef>
#include <cstdint>
#include "esp_err.h"
using nvs_handle_t = std::uint32_t;
enum nvs_open_mode_t { NVS_READONLY, NVS_READWRITE };
constexpr esp_err_t ESP_ERR_NVS_NOT_FOUND = 7;
esp_err_t nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*);
esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t, const char*);
esp_err_t nvs_erase_all(nvs_handle_t);
""",
                encoding="utf-8",
            )
            (include / "nvs_flash.h").write_text(
                """#pragma once
#include "esp_partition.h"
esp_err_t nvs_flash_init();
esp_err_t nvs_flash_init_partition(const char*);
esp_err_t nvs_flash_init_partition_ptr(const esp_partition_t*);
esp_err_t nvs_flash_erase();
esp_err_t nvs_flash_erase_partition(const char*);
esp_err_t nvs_flash_erase_partition_ptr(const esp_partition_t*);
""",
                encoding="utf-8",
            )
            harness = work / "test.cpp"
            harness.write_text(
                """#include <cstddef>
#include <cstdint>
#include <cstring>
#include "esp_partition.h"
#include "nvs.h"
#include "nvs_flash.h"
extern "C" bool sigap_ble_diag_failed() noexcept;
extern "C" esp_err_t __wrap_nvs_flash_init();
extern "C" esp_err_t __wrap_nvs_flash_init_partition(const char*);
extern "C" esp_err_t __wrap_nvs_flash_init_partition_ptr(const esp_partition_t*);
extern "C" esp_err_t __wrap_nvs_flash_erase();
extern "C" esp_err_t __wrap_nvs_flash_erase_partition(const char*);
extern "C" esp_err_t __wrap_nvs_flash_erase_partition_ptr(const esp_partition_t*);
extern "C" esp_err_t __wrap_nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*);
extern "C" esp_err_t __wrap_nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*);
extern "C" esp_err_t __wrap_nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t);
extern "C" esp_err_t __wrap_nvs_commit(nvs_handle_t);
extern "C" esp_err_t __wrap_nvs_erase_key(nvs_handle_t, const char*);
extern "C" esp_err_t __wrap_nvs_erase_all(nvs_handle_t);
extern "C" esp_err_t __wrap_esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t);
namespace {
int failures = 0, erase_calls = 0, nvs_calls = 0;
esp_err_t init_result = ESP_OK, partition_init_result = ESP_OK;
esp_err_t partition_ptr_init_result = ESP_OK, open_result = ESP_OK;
esp_err_t read_result = ESP_ERR_NVS_NOT_FOUND, set_result = ESP_OK;
esp_err_t commit_result = ESP_OK, erase_result = ESP_OK;
void expect(bool b) { if (!b) ++failures; }
}
extern "C" esp_err_t __real_nvs_flash_init() { return init_result; }
extern "C" esp_err_t __real_nvs_flash_init_partition(const char*) { return partition_init_result; }
extern "C" esp_err_t __real_nvs_flash_init_partition_ptr(const esp_partition_t*) { return partition_ptr_init_result; }
extern "C" esp_err_t __real_nvs_open(const char*, nvs_open_mode_t, nvs_handle_t*) { ++nvs_calls; return open_result; }
extern "C" esp_err_t __real_nvs_get_blob(nvs_handle_t, const char*, void*, std::size_t*) { ++nvs_calls; return read_result; }
extern "C" esp_err_t __real_nvs_set_blob(nvs_handle_t, const char*, const void*, std::size_t) { ++nvs_calls; return set_result; }
extern "C" esp_err_t __real_nvs_commit(nvs_handle_t) { ++nvs_calls; return commit_result; }
extern "C" esp_err_t __real_esp_partition_erase_range(const esp_partition_t*, std::size_t, std::size_t) { ++erase_calls; return erase_result; }
int main(int argc, char** argv) {
  if (argc > 1) {
    const char* scenario = argv[1];
    if (std::strcmp(scenario, "init") == 0) {
      init_result = ESP_FAIL;
      expect(__wrap_nvs_flash_init() == ESP_FAIL && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "partition-init") == 0) {
      partition_init_result = ESP_FAIL;
      expect(__wrap_nvs_flash_init_partition("nvs") == ESP_FAIL && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "partition-ptr-init") == 0) {
      partition_ptr_init_result = ESP_FAIL;
      esp_partition_t nvs{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs", 0x5000U};
      expect(__wrap_nvs_flash_init_partition_ptr(&nvs) == ESP_FAIL && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "open") == 0) {
      open_result = ESP_FAIL;
      expect(__wrap_nvs_open("sigap_ble", NVS_READONLY, nullptr) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "phy-open") == 0) {
      expect(__wrap_nvs_open("phy", NVS_READONLY, nullptr) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && !sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "phy-store-open") == 0) {
      expect(__wrap_nvs_open("phy", NVS_READWRITE, nullptr) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && !sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "read") == 0) {
      read_result = ESP_FAIL;
      expect(__wrap_nvs_get_blob(1U, "setup_secret", nullptr, nullptr) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "write") == 0) {
      set_result = ESP_FAIL;
      expect(__wrap_nvs_set_blob(1U, "bond", nullptr, 0U) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "commit") == 0) {
      commit_result = ESP_FAIL;
      expect(__wrap_nvs_commit(1U) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "erase") == 0) {
      erase_result = ESP_FAIL;
      esp_partition_t nvs{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs", 0x5000U};
      expect(__wrap_esp_partition_erase_range(&nvs, 0x1000U, 0x1000U) == ESP_ERR_INVALID_STATE);
      expect(erase_calls == 0 && sigap_ble_diag_failed());
    } else if (std::strcmp(scenario, "missing-entry") == 0) {
      expect(__wrap_nvs_get_blob(1U, "missing", nullptr, nullptr) == ESP_ERR_INVALID_STATE &&
             nvs_calls == 0 && sigap_ble_diag_failed());
    } else {
      return 2;
    }
    return failures;
  }
  expect(__wrap_nvs_flash_init() == ESP_OK && !sigap_ble_diag_failed());
  expect(__wrap_nvs_get_blob(1U, "missing", nullptr, nullptr) == ESP_ERR_INVALID_STATE);
  expect(nvs_calls == 0 && sigap_ble_diag_failed());
  esp_partition_t nvs{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "nvs", 0x5000U};
  for (std::size_t offset = 0U; offset < nvs.size; offset += 0x1000U) {
    expect(__wrap_esp_partition_erase_range(&nvs, offset, 0x1000U) == ESP_ERR_INVALID_STATE);
    expect(erase_calls == 0 && sigap_ble_diag_failed());
  }
  esp_partition_t other{ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_NVS, "other", 0x5000U};
  expect(__wrap_esp_partition_erase_range(&other, 0x1000U, 0x1000U) == ESP_ERR_INVALID_STATE);
  expect(erase_calls == 0);
  expect(__wrap_esp_partition_erase_range(&nvs, 0U, 0x5000U) == ESP_ERR_INVALID_STATE);
  expect(erase_calls == 0 && sigap_ble_diag_failed());
  expect(__wrap_nvs_flash_erase() == ESP_ERR_INVALID_STATE);
  expect(__wrap_nvs_flash_erase_partition("nvs") == ESP_ERR_INVALID_STATE);
  expect(__wrap_nvs_flash_erase_partition_ptr(&nvs) == ESP_ERR_INVALID_STATE);
  expect(__wrap_nvs_erase_key(1U, "bond") == ESP_ERR_INVALID_STATE);
  expect(__wrap_nvs_erase_all(1U) == ESP_ERR_INVALID_STATE);
  return failures;
}
""",
                encoding="utf-8",
            )
            binary = work / "nvs-guards-test"
            command = [
                "g++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                f"-I{include}", str(FIRMWARE / "src/diagnostic_nvs_guards.cpp"),
                str(harness), "-o", str(binary),
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            scenarios = (
                "init", "partition-init", "partition-ptr-init", "open",
                "phy-open", "phy-store-open", "read", "write", "commit",
                "erase", "missing-entry",
            )
            for scenario in scenarios:
                subprocess.run(
                    [str(binary), scenario], check=True, capture_output=True, text=True
                )
            print(f"NVS guard fixtures: {len(scenarios)}/{len(scenarios)} passed")


if __name__ == "__main__":
    unittest.main()
