# NimBLE bond-restore regression

SIGAP pins NimBLE-Arduino 2.5.1. Every target environment extending
`sigap_n16r8` runs `scripts/patch_nimble_restore.py` before the build. The hook
checks both `library.properties` and the complete source SHA-256, applies the
small restore guard only to the reviewed upstream source, verifies the patched
hash, and rejects unknown version/source drift. The dependency itself remains
in PlatformIO's ignored `.pio` cache; this repository-owned hook reapplies the
patch after a clean dependency install.

From `firmware/`, run:

```sh
pio run -e sigap_n16r8
SIGAP_NIMBLE_ROOT=.pio/libdeps/sigap_n16r8/NimBLE-Arduino \
  python -m unittest test/host/test_nimble_restore_patch.py -v
python test/host/nimble_restore/run.py \
  --nimble-root .pio/libdeps/sigap_n16r8/NimBLE-Arduino
```

The final command compiles the actual installed `ble_store_nvs.c` with GCC
ASan/UBSan and host-only NVS/OS shims. Its fixtures cover empty, both-side,
our-only, peer-only, and read-error restores; NVS writes are stubbed and counted
so the test cannot access device storage. This is host evidence, not ESP32
runtime evidence.

The patch changes only zero-count bond restore bookkeeping and does not enable
bond storage. All ESP32 firmware environments compile with bond persistence and
maximum bonds set to zero. The diagnostic target rejects NVS record opens and
all erase requests. The SDK's optional `phy` calibration
namespace open is rejected without raising the application failure latch so the
controller can follow its full-calibration fallback; that path is unverified on
hardware. The host fixture isolates the pinned
NimBLE restore implementation with synthetic records and cannot access device
storage. Historical policy and binary results in `verification-2026-10-17.md`
refer to an earlier authenticated one-bond configuration and are superseded for
the current no-passkey prototype. No physical NVS condition is inferred.
