"""Contract tests for the pinned NimBLE-Arduino build patch."""
from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest

PATCHER_PATH = Path(__file__).resolve().parents[2] / "scripts" / "patch_nimble_restore.py"
SPEC = importlib.util.spec_from_file_location("patch_nimble_restore", PATCHER_PATH)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot load patcher at {PATCHER_PATH}")
PATCHER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(PATCHER)


class NimbleRestorePatchTests(unittest.TestCase):
    def test_transform_guards_empty_sides_and_keeps_restored_counts(self) -> None:
        original = (
            "    ble_store_config_our_bond_count = "
            "ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count;\n"
            "    ble_store_config_peer_bond_count = "
            "ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count;"
        )
        patched = PATCHER.patch_source_text(original)
        self.assertIn("ble_store_config_num_our_secs > 0", patched)
        self.assertIn("ble_store_config_num_peer_secs > 0", patched)
        self.assertIn("ble_store_config_our_bond_count = 0;", patched)
        self.assertIn("ble_store_config_peer_bond_count = 0;", patched)
        self.assertIn(
            "ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count",
            patched,
        )
        self.assertIn(
            "ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count",
            patched,
        )

    def test_transform_is_idempotent(self) -> None:
        original = PATCHER.OLD_BOND_COUNT_ASSIGNMENTS.decode("ascii")
        patched = PATCHER.patch_source_text(original)
        self.assertEqual(PATCHER.patch_source_text(patched), patched)

    def test_diagnostic_repeat_pairing_refuses_replacement_without_delete(self) -> None:
        source = """        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* We already have a bond with the peer, but it is attempting to
             * establish a new secure link.  This app sacrifices security for
             * convenience: just throw away the old bond and accept the new link.
             */

            /* Delete the old bond. */
            rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &peerInfo.m_desc);
            if (rc != 0) {
                return BLE_GAP_REPEAT_PAIRING_IGNORE;
            }

            ble_store_util_delete_peer(&peerInfo.m_desc.peer_id_addr);

            /* Return BLE_GAP_REPEAT_PAIRING_RETRY to indicate that the host should
             * continue with the pairing operation.
             */
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        } // BLE_GAP_EVENT_REPEAT_PAIRING"""
        patched = PATCHER.patch_repeat_pairing_text(source)
        self.assertIn("return BLE_GAP_REPEAT_PAIRING_IGNORE;", patched)
        self.assertNotIn("ble_store_util_delete_peer", patched)
        self.assertNotIn("BLE_GAP_REPEAT_PAIRING_RETRY", patched)
        self.assertEqual(PATCHER.patch_repeat_pairing_text(patched), patched)

    def test_installed_diagnostic_server_is_patched_and_hash_checked(self) -> None:
        root_value = os.environ.get("SIGAP_NIMBLE_ROOT")
        if not root_value:
            self.skipTest("set SIGAP_NIMBLE_ROOT to an installed NimBLE-Arduino library")
        installed = Path(root_value).resolve()
        with tempfile.TemporaryDirectory(prefix="sigap-repeat-pairing-test-") as temp:
            clone = Path(temp) / "NimBLE-Arduino"
            clone.mkdir()
            shutil.copy2(installed / "library.properties", clone / "library.properties")
            target = clone / PATCHER.SERVER_SOURCE_RELATIVE
            target.parent.mkdir(parents=True)
            shutil.copy2(installed / PATCHER.SERVER_SOURCE_RELATIVE, target)

            status = PATCHER.patch_nimble_diagnostic_server(clone)
            self.assertIn(status, ("patched", "already-patched"))
            self.assertEqual(PATCHER.sha256_file(target), PATCHER.PATCHED_SERVER_SHA256)
            text = target.read_text(encoding="utf-8")
            self.assertNotIn("ble_store_util_delete_peer", text)
            self.assertEqual(PATCHER.patch_nimble_diagnostic_server(clone), "already-patched")

    def test_unknown_source_drift_is_rejected(self) -> None:
        with self.assertRaises(PATCHER.PatchError):
            PATCHER.patch_source_text("/* unrecognized NimBLE source */\n")

    def test_pinned_library_patch_and_repeat_are_idempotent(self) -> None:
        root_value = os.environ.get("SIGAP_NIMBLE_ROOT")
        if not root_value:
            self.skipTest("set SIGAP_NIMBLE_ROOT to an installed NimBLE-Arduino library")
        installed = Path(root_value).resolve()
        source = installed / PATCHER.SOURCE_RELATIVE
        properties = installed / "library.properties"
        self.assertTrue(source.is_file(), source)
        self.assertTrue(properties.is_file(), properties)

        with tempfile.TemporaryDirectory(prefix="sigap-patch-test-") as temp:
            clone = Path(temp) / "NimBLE-Arduino"
            clone.mkdir()
            shutil.copy2(properties, clone / "library.properties")
            target_source = clone / PATCHER.SOURCE_RELATIVE
            target_source.parent.mkdir(parents=True)
            shutil.copy2(source, target_source)

            first = PATCHER.patch_nimble_library(clone)
            self.assertIn(first, ("patched", "already-patched"))
            self.assertEqual(
                PATCHER.sha256_file(target_source), PATCHER.PATCHED_SOURCE_SHA256
            )
            second = PATCHER.patch_nimble_library(clone)
            self.assertEqual(second, "already-patched")

    def test_changed_dependency_source_is_rejected(self) -> None:
        root_value = os.environ.get("SIGAP_NIMBLE_ROOT")
        if not root_value:
            self.skipTest("set SIGAP_NIMBLE_ROOT to an installed NimBLE-Arduino library")
        installed = Path(root_value).resolve()
        with tempfile.TemporaryDirectory(prefix="sigap-drift-test-") as temp:
            clone = Path(temp) / "NimBLE-Arduino"
            clone.mkdir()
            shutil.copy2(installed / "library.properties", clone / "library.properties")
            target_source = clone / PATCHER.SOURCE_RELATIVE
            target_source.parent.mkdir(parents=True)
            shutil.copy2(installed / PATCHER.SOURCE_RELATIVE, target_source)
            target_source.write_bytes(target_source.read_bytes() + b"\n/* drift */\n")
            with self.assertRaises(PATCHER.PatchError):
                PATCHER.patch_nimble_library(clone)

    def test_wrong_library_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory(prefix="sigap-version-test-") as temp:
            clone = Path(temp) / "NimBLE-Arduino"
            clone.mkdir()
            (clone / "library.properties").write_text(
                "name=NimBLE-Arduino\nversion=2.5.2\n", encoding="utf-8"
            )
            target_source = clone / PATCHER.SOURCE_RELATIVE
            target_source.parent.mkdir(parents=True)
            target_source.write_bytes(b"source contents are irrelevant to version check")
            with self.assertRaises(PATCHER.PatchError):
                PATCHER.patch_nimble_library(clone)


if __name__ == "__main__":
    unittest.main()
