# Task 9 BLE diagnostic physical run (2026-10-18)

> Historical physical run against the earlier authenticated BLE image, SHA-256 `b7913a74db474be9e56691d98a0634132bce2cb6a8481efedf24b389e2375630`. The observations below remain unchanged and apply only to that run, not to the current no-passkey source. Current host-only evidence is in [task9-ble-no-passkey-verification.md](task9-ble-no-passkey-verification.md).

## Authorization and image identity

The owner explicitly selected: `Izinkan preflight, flash aplikasi saja, dan uji BLE terbatas`.
The reviewed scope included application-only flash, bounded boot,
necessary `nimble_bond` namespace creation and bond/CCCD persistence without
NVS erase. Existing setup secret was to be used internally, never recreated,
rotated or read out. Sensors, actuators, SD, Wi-Fi provisioning and profile
mutations remained excluded. This is scoped run authorization, not perpetual
permission for another flash, reset, bond deletion or erase.

Software candidate and immutable bundle:
`/home/raya/.hermes/artifacts/sigap-ble-strict-no-erase-20261018-095147/`.
Physical evidence:
`/home/raya/.hermes/artifacts/sigap-ble-physical-20261018-101711/`.
Repository branch `feat/full-integration-hil-validation`, HEAD
`0257b9e5cc08b142348f85f6da184842c09ab420`; no commit, push or merge.

Installed application SHA-256:
`b7913a74db474be9e56691d98a0634132bce2cb6a8481efedf24b389e2375630`.
Image size is 496080 bytes. The earlier permissive-GC candidate was not flashed.
NimBLE-Arduino remains 2.5.1 with the reviewed local hash-guarded patches.

## Verified physical steps

1. The current USB JTAG/serial port was `/dev/ttyACM0`, with USB identity
   `303A:1001` and serial/MAC `98:A3:16:F5:C1:50`. No holder was reported by
   the preflight port check. Only the lead owned board operations.
2. esptool 4.11.0 entered the ROM bootloader through its USB reset strategy,
   with `--no-stub` and `--after no_reset`. It identified ESP32-S3 QFN56 v0.2,
   embedded 8 MB PSRAM, MAC `98:a3:16:f5:c1:50`, 16 MB quad flash, secure boot
   disabled and flash encryption disabled. Application boot was not requested
   during these checks.
3. Only the 4096-byte partition table at `0x8000` was read for layout checks.
   The pinned SDK partition utility verified it: NVS `0x9000/0x5000`, PHY
   `0xe000/0x1000`, factory app `0x10000/0x400000`, filesystem
   `0x410000/0x800000`. No NVS bytes or setup-secret values were read out.
4. The single write command contained only `0x10000 firmware.bin`, with
   flash mode/frequency/size kept, no force or erase-all option. esptool
   reported application-sector erase `0x10000..0x89fff` and verified the data
   hash. Its transport padded the write to 496640 bytes; the image remains
   496080 bytes. No bootloader, partition-table, NVS or filesystem image was
   supplied to the flasher.
5. The 499712-byte sector-rounded app region was read back before boot.
   Its first 496080 bytes matched the candidate byte-for-byte, with the same
   SHA-256 above. The partition table read after upload also matched its
   pre-upload bytes exactly. These checks do not measure raw NVS preservation.
6. A local observer performed one application boot and observed for 10.432 s.
   It received the exact `BLE_DIAG_READY wifi=DISABLED status=device_identity_bound`
   marker, with no stop reason or panic/assert/reboot marker in that bounded
   window. Raw serial lines were classified then discarded; no passkey command
   was sent. An earlier host observer import error happened before the serial
   port was opened or a boot action was made; the pinned tool's vendored
   dependency path was then applied and self-tested before the actual boot.

The boot result validates this diagnostic startup window, not production
sensor behavior, endurance, byte-for-byte NVS preservation or full Task 9.

## BLE observations and exact blocker

The host's stored device record already listed `98:A3:16:F5:C1:51` as paired,
bonded and trusted. This was cache/security metadata, not authentication proof.
No pair/unpair API was called by the probes, and no passkey was requested or
read by the scripts. The tests used Bleak 3.0.2, dbus-fast 5.2.0 and pyserial
3.5 in an isolated evidence-directory venv, not the system environment.

The first address-based Bleak probe ended in `BleakDeviceNotFoundError` during
implicit discovery. A separate six-second discovery-only observation, with no
UUID discovery filter and no connection, then observed `SIGAP-A398` at the exact
expected address. Only target/SIGAP candidate metadata was retained. This is
bounded discovery evidence, not GATT identity verification.

A revised probe used a freshly discovered `BLEDevice` object rather than
triggering another implicit scan from an address. It still failed during the
initial connection. One instrumented diagnostic established the narrow phase:
`connect-and-resolve-services`. Bleak's `_wait_for_services_discovery` raised
`failed to discover services, device disconnected` at the installed manager's
line 888. The logged categories and stack contain no raw credential values.
The instrumented run did not reach notification setup or application commands.
No successful GET_DEVICE_ID or GET_STATUS response was obtained; the reconnect
identity phase did not run.

Serial monitoring during the probes observed no diagnostic stop or crash
marker. Bluetooth service remained active/running, with `NRestarts=0` and
`ExecMainStatus=0`; the adapter was powered and not rfkill-blocked in the host
check. The final cached target state was disconnected with its bond retained.
Those facts do not establish a cause for the link drop. An authentication or
bond mismatch is only a hypothesis, not proof that the bond is stale, invalid
or disposable. No bond was deleted, replaced or evicted; no authentication
requirement was weakened, and no reset/upload/service restart was used as a
response to this failure. Further connection retries stopped after the bounded
instrumented diagnostic.

## Evidence and remaining gate

Key files in the physical evidence directory:

- `security-preflight.log`, `flash-identity.log`, `preflight-summary.json`
- `partition-table.bin`, `partition-table-decoded.log`, `partition-table-after.bin`
- `app-only-write.log`, `app-readback.bin`, `flash-readback-summary.json`
- `boot-observation-v2-sanitized.log`, `tests/test_boot_observe.py`
- `name-discovery.log`, `tests/test_name_discovery.py`
- `authenticated-gatt-run.log`, `authenticated-gatt-v2-run.log`,
  `authenticated-gatt-v3-run.log` and their versioned probe sources
- `physical-summary.json`, `host-final-state.log`, `host-test-requirements.txt`

```text
BOARD_PREFLIGHT=PASS
APP_ONLY_FLASH=PASS
APP_READBACK=PASS_BYTE_FOR_BYTE
PARTITION_TABLE_UNCHANGED=PASS
BLE_DIAGNOSTIC_BOOT=PASS_BOUNDED_10_SECONDS
BLE_NAME_ADDRESS_DISCOVERY=PASS_BOUNDED_6_SECONDS
BLE_SERVICE_DISCOVERY=BLOCKED_LINK_DISCONNECT
BLE_MITM_AUTHENTICATED=NOT_VERIFIED
REAL_GET_DEVICE_ID=NOT_VERIFIED
REAL_GET_STATUS=NOT_VERIFIED
BLE_RECONNECT=NOT_RUN
FRESH_PAIRING=NOT_RUN
BOND_DELETION=NOT_RUN
NVS_RAW_PRESERVATION=NOT_MEASURED_HISTORICAL_FAIL_UNCHANGED
TASK9_STATUS=HOLD
```

The next prerequisite is credential-free evidence of the controller/SMP
reason for the disconnect during service discovery. Do not infer stale bond
from the generic host error, repeat the upload, erase NVS, delete a bond or
relax MITM. Any proposed reset, new image, bond change or privileged host-policy
change must be reviewed against the current scope before execution.
