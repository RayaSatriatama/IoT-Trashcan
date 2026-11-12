# Task 9 NVS authorization audit

Date: 2026-10-17. Audited source HEAD:
`0257b9e5cc08b142348f85f6da184842c09ab420`.

This is source inspection plus host-only testing, not a physical NVS audit.
The lead reviewed the Luna audit artifacts, inspected the relevant application,
installed Arduino/NimBLE sources, and independently reran both focused checks.
No firmware, dependency, NVS, bond, network policy, or hardware was changed.
Only documentation was integrated. Task 9 remains HOLD; PR #27 remains unmerged.
There is no Task 10 in the current owner scope.

## Authority and evidence limits

PRD firmware AC31-AC34 and secret/provisioning ownership, TSD sections 3A.1,
3A.4 and 3A.5, frozen WEB-CAL security contracts, current owner authorization,
and the latest verification ledger remain authoritative. Upstream pool-controller
`config-nvs` skill examples are not SIGAP APIs or authorization.

The existing physical NVS partition hash mismatch remains
`NVS_PRESERVED=FAIL`. The changed physical records were not decoded. Older
statements attributing the entire mismatch to creation of `setup_secret` are
superseded as an exclusive causal conclusion: that is a verified source write
candidate, not the only possible mutation or a proven inventory of that boot.
There is no new evidence of NVS corruption, credential loss, erase, or bond loss.

## Setup-secret lifecycle

Application anchors:

- `firmware/src/main.cpp:27-29`: sketch `setup()` delegates to `SigapApp`.
- `firmware/src/app/sigap_app.cpp:350-383`: Wi-Fi begin precedes BLE begin;
  the application ignores the BLE service begin return at line 381.
- `firmware/lib/provisioning/src/provisioning/ble_provisioning.cpp:680-701`:
  `NimBLEDevice::init()` runs before the secret-store load; its return is also
  ignored. Existing MITM, bonding, Secure Connections and authenticated GATT
  permissions are preserved.
- `firmware/lib/provisioning/src/provisioning/provisioning_secret_store.cpp:35-61`:
  namespace `sigap_ble`, key `setup_secret`, opened `NVS_READWRITE`.

The secret store uses an existing exact-size 32-byte blob without generating
random bytes, calling `nvs_set_blob()`, or calling `nvs_commit()` in that
function. Only `ESP_ERR_NVS_NOT_FOUND` creates a new value. Wrong size/type,
read/open/set/commit errors fail closed and zero the output, rather than
silently rotating an existing value. This proves a source control-flow rule,
not that the physical board currently contains a valid key or that a normal
restart preserves the entire partition byte-for-byte.

Read-write open can create an absent namespace before the key lookup. Matching
ESP-IDF v4.4.7 source confirms `open_handle()` permits namespace creation in RW
mode and `createOrOpenNamespace()` writes a namespace index when absent.
Its `set_blob()` calls `writeItem()` directly; `commit()` does not implement
transaction staging/rollback. Never infer physical rollback from a mocked
commit failure.

## Other startup and command-triggered mutation paths

Installed dependencies below are local source evidence, not identification of
which branches ran in the previously uploaded out-of-tree image. NimBLE was
verified as 2.5.1 in the installed `library.properties`; PlatformIO configuration
pins that version and limits the target to one bond and two CCCDs.

| Area | Trigger / source behavior | Source anchor |
| --- | --- | --- |
| Arduino NVS initialization | `initArduino()` precedes sketch setup. On `ESP_ERR_NVS_NO_FREE_PAGES` or `ESP_ERR_NVS_NEW_VERSION_FOUND`, the framework can erase the matching NVS partition and retry. No evidence that this happened on the board. | Installed `cores/esp32/main.cpp:55-72`, `cores/esp32/esp32-hal-misc.c:249-265` |
| NimBLE initialization | Another conditional NVS erase/re-init fallback on the same initialization errors; store restore then opens `nimble_bond` RW, potentially creating an absent namespace. | Installed `NimBLEDevice.cpp:904-915,1008`; `ble_store_config.c:1163-1184`; `ble_store_nvs.c:163-180,922-935` |
| BLE security / subscriptions | Pairing can persist security records; enabled CCCD/CSFC/privacy/identity paths can persist additional records. Delete callbacks exist, although no application bond-delete call was found. Do not assume an authenticated read session has zero persistent side effects. | Installed `ble_store_nvs.c:39-52,320-378,669-699`; `ble_store_config.c:1123-1184` |
| Wi-Fi driver configuration | Arduino persistence defaults to true; driver storage defaults to FLASH. Application `WiFi.begin()` can reach `esp_wifi_set_config()`. No production `WiFi.persistent(false)` call was found. Whether the observed boot changed these records is unknown. | `firmware/lib/provisioning/src/provisioning/wifi_manager.cpp:20-51,237-246`; installed `WiFiGeneric.cpp:658-695,763`, `WiFiSTA.cpp:241-267`, SDK `esp_wifi.h:880-892` |
| Application credentials | Explicit USB secret operations or a successful BLE Wi-Fi candidate can persist `sigap_secret`; boot network credential load is read-only. Secret clear does not delete the BLE setup secret or bonds. | `secret_store.cpp:250-297,370-429`; `wifi_manager.cpp:54-75`; `maintenance/runtime.cpp:576-584` |
| Configuration | Boot slot/activation reads are read-only. Explicit config commit persists slots and activation/provisioned metadata. Config reset changes a draft, not an immediate persistent commit. | `config/nvs_config_store.cpp:17-20,79-91,154-206`; `maintenance/runtime.cpp:307-320` |
| Acquisition profiles / models | Boot loads use read-only handles. Explicit commit/rollback/reset operations persist the applicable slot or activation metadata; model cancel clears RAM draft. | `acquisition/profile_store.cpp:894-1027`; `models/model_lifecycle.cpp:445-571,707-742`; `sigap_app.cpp:389-408` |
| Calibration replay | COMMIT prepares a durable intent and persists terminal results for nonzero request IDs; not a read-only status operation. | `calibration/calibration_transport.cpp:233-273`; `calibration/calibration_replay_store.cpp:181-255` |

Paths in this table are firmware library-relative unless otherwise stated.
The mere presence of a conditional erase or write path does not prove execution.
Do not repair this evidence gap by rebooting, flashing, deleting a bond, or
reading a secret without the corresponding explicit authorization.

## NimBLE empty-store source risk

The installed `ble_store_nvs.c:458-539` restores only keys that exist; missing
keys leave the initialized record counts at zero. At lines 587-588,
`ble_nvs_restore_sec_keys()` indexes the security arrays using `num_secs - 1`
without guarding zero counts. The arrays and signed integer counts are defined
in `ble_store_config.c:30-44`. The same expressions are present in upstream
NimBLE-Arduino 2.5.1.

This is a source-level out-of-bounds read risk on an empty security-store path.
It is not a reproduced target crash, a cause established for the host BlueZ
SIGSEGV, or evidence of the board's current record count. The two focused
checks below do not compile/exercise that installed NimBLE C restore path.
Review the path before a new boot/pairing campaign; do not delete a bond to
create a test case. Any remediation requires its own regression and dependency
integration review, not an untracked edit inside `.pio/libdeps`.

## Fresh lead verification

From `firmware/`:

```sh
/home/raya/.local/bin/pio test -e native -f native/test_ble_provisioning
```

PlatformIO Core 6.2.0: exit 0, 15 tests passed. This suite uses
`MemoryProvisioningSecretStore`; it does not verify hardware NVS or NimBLE's
C persistence implementation. No unrelated firmware/Go/frontend suites were
rerun for documentation-only integration.

The lead also compiled and ran the actual production
`provisioning_secret_store.cpp` with `-DARDUINO`, `-Wall -Wextra -Werror`, and
the audit's local mock headers/calls. Command:

```sh
AUDIT=/home/raya/.hermes/cache/scratch/sigap-nvs-audit-80trmox5
SRC=/mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation/firmware/lib/provisioning/src
g++ -std=c++17 -DARDUINO -Wall -Wextra -Werror \
  -I"$AUDIT/harness/include" -I"$SRC" \
  "$SRC/provisioning/provisioning_secret_store.cpp" \
  "$AUDIT/harness/mock_nvs.cpp" "$AUDIT/harness/test_secret_store.cpp" \
  -o "$AUDIT/harness/test_secret_store_lead"
"$AUDIT/harness/test_secret_store_lead"
```

Compilation/execution exited 0, with `RESULT cases=8 passed=8 failed=0`:
create, reuse after store-object recreation, short blob, wrong type, read error,
open error, set error, and commit error. Failure output zeroization and absence
of silent rotation are checked. No value or passkey is printed.

The mock stages writes until commit, unlike the matched IDF implementation.
Thus the final case proves only the function's mocked error handling, not that
hardware flash remains unchanged after a failed commit. Namespace creation,
flash/GC behavior, entropy, bond persistence, and physical restart were not
verified. Scratch artifacts are temporary; preserve them if reproducing these
commands later. Original report and logs are in the `AUDIT` directory above.

## Separate owner decisions and stopping rules

A. Accept retention/reuse of the existing `sigap_ble/setup_secret`, without
   readout, recreation, rotation, deletion, or rewrite. This does not retroactively
   change `NVS_PRESERVED=FAIL`. Presence/validity on the current board is still
   unverified by this audit; current startup would create the key if absent.

B. Separately authorize necessary target-specific BLE security/subscription
   persistence for bounded authenticated `GET_DEVICE_ID` / `GET_STATUS` and a
   disconnect/reconnect identity revalidation, after reviewing the restore risk
   above and the exact test procedure. Do not authorize bond deletion,
   replacement/eviction, Wi-Fi/token provisioning, reset/flash, or profile commit
   implicitly. Stop rather than clear a full one-bond store or permit an
   unexpected deletion request. A and B do not imply one another.

General hardware access permission excludes sensors/actuators and does not
waive the preceding specific mutation limits. The current firmware has no
metadata-only NVS endpoint. A hypothetical `NVS_READONLY` presence/type/length
check is not presently an executable approved procedure: implementing or
booting a diagnostic image has its own flash/reset/startup side effects.

Until the required decisions and source-risk review are resolved:

```text
NVS_SOURCE_AUDIT=PASS (reviewed source and bounded host checks)
NVS_PRESERVED=FAIL (historical physical invariant; unchanged)
NVS_AUTHORIZATION=BLOCKED
BLE_RESTORE_EMPTY_STORE_RISK=SOURCE_IDENTIFIED_NOT_TARGET_REPRODUCED
BLE_MITM_AUTHENTICATED=NOT_RUN
REAL_GET_DEVICE_ID=NOT_RUN
REAL_GET_STATUS=NOT_RUN
BLE_RECONNECT=NOT_RUN
TASK9_STATUS=HOLD
```

## Version-matched primary sources checked by the lead

- [ESP-IDF v4.4.7 namespace open](https://github.com/espressif/esp-idf/blob/v4.4.7/components/nvs_flash/src/nvs_partition_manager.cpp)
- [ESP-IDF v4.4.7 namespace creation](https://github.com/espressif/esp-idf/blob/v4.4.7/components/nvs_flash/src/nvs_storage.cpp)
- [ESP-IDF v4.4.7 blob write and commit](https://github.com/espressif/esp-idf/blob/v4.4.7/components/nvs_flash/src/nvs_handle_simple.cpp)
- [NimBLE-Arduino 2.5.1 NVS store](https://github.com/h2zero/NimBLE-Arduino/blob/2.5.1/src/nimble/nimble/host/store/config/src/ble_store_nvs.c)
