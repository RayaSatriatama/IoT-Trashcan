"""Hash-guarded SIGAP patch for NimBLE-Arduino 2.5.1 bond restore."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import tempfile

EXPECTED_VERSION = "2.5.1"
EXPECTED_LIBRARY_NAME = "NimBLE-Arduino"
SOURCE_RELATIVE = Path(
    "src/nimble/nimble/host/store/config/src/ble_store_nvs.c"
)
ORIGINAL_SOURCE_SHA256 = "e4d61d3b6403e263d40f2f498c4d6ac64ee1727ccd3fd6f040ba0e1908c26144"
PATCHED_SOURCE_SHA256 = "b5f7d79dcbdc568977f1f93dfe12f7edd046ce832f2eeab8056e4ad81b366bd3"
SERVER_SOURCE_RELATIVE = Path("src/NimBLEServer.cpp")
ORIGINAL_SERVER_SHA256 = "af4335cf6b9e5ca3be63770307736fe50d29b53529c0dd3579f7ed515b553895"
PATCHED_SERVER_SHA256 = "da2bcef89c0f18d913a31cd401d02412a096e0e0797c26f2842ae3487c99a65e"

OLD_BOND_COUNT_ASSIGNMENTS = (
    b"    ble_store_config_our_bond_count = "
    b"ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count;\n"
    b"    ble_store_config_peer_bond_count = "
    b"ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count;"
)
NEW_BOND_COUNT_ASSIGNMENTS = (
    b"    ble_store_config_our_bond_count = 0;\n"
    b"    if (ble_store_config_num_our_secs > 0) {\n"
    b"        ble_store_config_our_bond_count =\n"
    b"            ble_store_config_our_secs[ble_store_config_num_our_secs - 1].bond_count;\n"
    b"    }\n"
    b"    ble_store_config_peer_bond_count = 0;\n"
    b"    if (ble_store_config_num_peer_secs > 0) {\n"
    b"        ble_store_config_peer_bond_count =\n"
    b"            ble_store_config_peer_secs[ble_store_config_num_peer_secs - 1].bond_count;\n"
    b"    }"
)

OLD_REPEAT_PAIRING = """        case BLE_GAP_EVENT_REPEAT_PAIRING: {
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
NEW_REPEAT_PAIRING = """        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // SIGAP diagnostic build never replaces an existing bond.
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        } // BLE_GAP_EVENT_REPEAT_PAIRING"""


class PatchError(RuntimeError):
    """The installed dependency does not match this reviewed patch."""


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def patch_source_text(source: str) -> str:
    """Apply the narrow textual edit once; reject partial/ambiguous states."""
    old = OLD_BOND_COUNT_ASSIGNMENTS.decode("ascii")
    new = NEW_BOND_COUNT_ASSIGNMENTS.decode("ascii")
    old_count = source.count(old)
    new_count = source.count(new)
    if old_count == 0 and new_count == 1:
        return source
    if old_count == 1 and new_count == 0:
        return source.replace(old, new, 1)
    raise PatchError(
        "expected exactly one original or already-patched bond-count block; "
        f"found original={old_count}, patched={new_count}"
    )


def patch_repeat_pairing_text(source: str) -> str:
    """Reject repeat pairing without the upstream bond-delete-and-retry path."""
    old_count = source.count(OLD_REPEAT_PAIRING)
    new_count = source.count(NEW_REPEAT_PAIRING)
    if old_count == 0 and new_count == 1:
        return source
    if old_count == 1 and new_count == 0:
        return source.replace(OLD_REPEAT_PAIRING, NEW_REPEAT_PAIRING, 1)
    raise PatchError(
        "expected exactly one original or diagnostic repeat-pairing block; "
        f"found original={old_count}, patched={new_count}"
    )


def _library_metadata(library_dir: Path) -> dict[str, str]:
    properties = library_dir / "library.properties"
    if not properties.is_file():
        raise PatchError(f"missing NimBLE library metadata: {properties}")
    metadata: dict[str, str] = {}
    for line in properties.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            metadata[key.strip()] = value.strip()
    if metadata.get("name") != EXPECTED_LIBRARY_NAME:
        raise PatchError(
            f"expected {EXPECTED_LIBRARY_NAME}, got {metadata.get('name')!r}"
        )
    if metadata.get("version") != EXPECTED_VERSION:
        raise PatchError(
            f"expected {EXPECTED_LIBRARY_NAME} {EXPECTED_VERSION}, "
            f"got {metadata.get('version')!r}"
        )
    return metadata


def _atomic_write(path: Path, content: bytes) -> None:
    temp_name = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="wb", dir=path.parent, prefix=path.name + ".", suffix=".tmp",
            delete=False,
        ) as temp_file:
            temp_name = temp_file.name
            temp_file.write(content)
            temp_file.flush()
            os.fsync(temp_file.fileno())
        os.replace(temp_name, path)
    finally:
        if temp_name is not None and os.path.exists(temp_name):
            os.unlink(temp_name)


def patch_nimble_library(library_dir: Path) -> str:
    """Verify version+source identity and patch the installed library safely."""
    library_dir = Path(library_dir)
    _library_metadata(library_dir)
    source = library_dir / SOURCE_RELATIVE
    if not source.is_file():
        raise PatchError(f"missing NimBLE restore source: {source}")

    source_hash = sha256_file(source)
    if source_hash == PATCHED_SOURCE_SHA256:
        # Also confirm that the patched block itself remains present exactly once.
        patch_source_text(source.read_text(encoding="utf-8"))
        return "already-patched"
    if source_hash != ORIGINAL_SOURCE_SHA256:
        raise PatchError(
            "NimBLE-Arduino source hash is not the reviewed 2.5.1 input or "
            f"SIGAP-patched output: {source_hash}"
        )

    original = source.read_bytes()
    try:
        patched = patch_source_text(original.decode("utf-8")).encode("utf-8")
    except UnicodeDecodeError as exc:
        raise PatchError("NimBLE restore source is not UTF-8") from exc
    patched_hash = hashlib.sha256(patched).hexdigest()
    if patched_hash != PATCHED_SOURCE_SHA256:
        raise PatchError(
            "reviewed patch output hash mismatch: "
            f"expected {PATCHED_SOURCE_SHA256}, got {patched_hash}"
        )

    _atomic_write(source, patched)
    actual_hash = sha256_file(source)
    if actual_hash != PATCHED_SOURCE_SHA256:
        raise PatchError(
            f"patched dependency readback mismatch: {actual_hash}"
        )
    return "patched"


def patch_nimble_diagnostic_server(library_dir: Path) -> str:
    """Apply the repeat-pairing rejection only to the diagnostic environment."""
    library_dir = Path(library_dir)
    _library_metadata(library_dir)
    source = library_dir / SERVER_SOURCE_RELATIVE
    if not source.is_file():
        raise PatchError(f"missing NimBLE server source: {source}")
    source_hash = sha256_file(source)
    if source_hash == PATCHED_SERVER_SHA256:
        patch_repeat_pairing_text(source.read_text(encoding="utf-8"))
        return "already-patched"
    if source_hash != ORIGINAL_SERVER_SHA256:
        raise PatchError(
            "NimBLE-Arduino server source is not the reviewed 2.5.1 input or "
            f"SIGAP diagnostic output: {source_hash}"
        )
    try:
        patched = patch_repeat_pairing_text(source.read_text(encoding="utf-8"))
    except UnicodeDecodeError as exc:
        raise PatchError("NimBLE server source is not UTF-8") from exc
    patched_bytes = patched.encode("utf-8")
    if hashlib.sha256(patched_bytes).hexdigest() != PATCHED_SERVER_SHA256:
        raise PatchError("reviewed diagnostic server patch output hash mismatch")
    _atomic_write(source, patched_bytes)
    if sha256_file(source) != PATCHED_SERVER_SHA256:
        raise PatchError("diagnostic server patch readback hash mismatch")
    return "patched"


def _run_platformio_hook() -> None:
    Import("env")  # noqa: F821 - injected by PlatformIO/SCons
    project_libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR"))  # noqa: F821
    pio_environment = env.subst("$PIOENV")  # noqa: F821
    library_dir = project_libdeps / pio_environment / EXPECTED_LIBRARY_NAME
    status = patch_nimble_library(library_dir)
    diagnostic_status = "not-applied"
    if pio_environment == "sigap_ble_diag":
        diagnostic_status = patch_nimble_diagnostic_server(library_dir)
    print(
        "SIGAP NimBLE restore patch: "
        f"{status}; {EXPECTED_LIBRARY_NAME} {EXPECTED_VERSION}; "
        f"source sha256={PATCHED_SOURCE_SHA256}; "
        f"repeat-pairing diagnostic patch={diagnostic_status}"
    )


# PlatformIO executes this as an extra script with SCons' Import helper present.
# Keeping ordinary imports side-effect free allows the focused unit tests to load it.
if "Import" in globals():
    _run_platformio_hook()
