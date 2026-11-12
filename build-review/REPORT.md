# Independent review — TLS worker stack correction

**Status: INCOMPLETE.** The released four-file source change, its documentation qualification, and existing target-build artifacts were reviewed read-only. The shared 16,384-byte policy is applied at the three HTTPS worker creation sites and both target artifacts are present and hash-verified. The required independent behavioral regression for all three production translation units remains outstanding; it was not inspected or run. This is not whole-task PASS or physical validation.

## Scope and source identities

Repository: `/mnt/nvme0n1p4/Projects/Programming_Projects/full-integration-hil-validation`  
HEAD: `0257b9e5cc08b142348f85f6da184842c09ab420` on `feat/full-integration-hil-validation`.

I compared the live source/config hashes against every entry in `final-source-manifest.json`; all nine entries matched. In particular, the four correction files and their live SHA-256 values are:

| File | SHA-256 |
|---|---|
| `firmware/lib/network/src/network/network.h` | `8bbf63bbefe5e3066ae7779800a09bb56ad5b154a1ab8183a3af5a299831c048` |
| `firmware/lib/network/src/network/https_transport.cpp` | `ac8d9d56b353baf1b6f64e3c28b231188983a5a545baf1bb7af4cd4f63f5e53e` |
| `firmware/lib/network/src/network/device_control_http.cpp` | `fee9264ab337e9f3347b9cea147450f1cafa353982b6c9de422d112ac6cec458` |
| `firmware/lib/debug/src/debug/http_transport.cpp` | `760102f0ce9296658005588eb2fe0432e3ce412fb5399223202e8e3abd852be8` |

The remaining manifest-matched inputs were `firmware/test/host/debug_http_transport/probe.cpp` (`336355fd8b13b70428e8ebb1be8f7698cd1ab9da8740f228ac3df20c2b2d25c5`), `tests/test_debug_http_transport.py` (`d3fe9b408d17dba7869e74a5b3a2cefead6fe3e36d1576de407ff302700917a2`), `docs/TASK9_Customization.md` (`1355d6a3e1f2a8c5bf392b36efd1e6e37227d7c15520eada43d5db2baa28f704`), `firmware/platformio.ini` (`ab2d71c981137b76dc4fda1c23b410e0635314e65d2cb4666c0948f320e98641`), and `firmware/scripts/patch_nimble_restore.py` (`d65b690de76e57a49aad9805a8e5c23207b42e2f233bdaa6f6fd43a552ab53e3`). The immutable released before-manifest/copies and final manifest reconcile as expected for these entries. The new regression-test paths were not read.

Source review verified one `inline constexpr uint32_t kTlsWorkerStackBytes{16384U}` in `network.h`; archive `sigap_net`, control `sigap_ctl`, and debug `sigap_dbg_http` each pass that constant to `xTaskCreate`. Control's misleading `StackWords` name/comment is removed. The observed call-site diffs retain priority `1U`, task argument/lifetime (`this`), and failure handling; no changes were observed to deadlines, retries, CA/token access, TLS validation, or callback ownership in these edits. This supports source-level scope preservation, not runtime equivalence proof.

Against the released-before documentation snapshot, the only documentation delta is a TLS stack-budget row: immutable 16,384-byte compile-time policy for all three workers, explicitly conservative and not measured safe, with target high-water/heap qualification required. Documentation does not represent that qualification as complete.

## Existing checks and build/artifact binding

Existing evidence was inspected; no builds or tests were started. `debug-http-test.log` reports one test, `OK`, `EXIT=0`; it is limited to the debug HTTP harness and does not verify three task-creation stack arguments. `build-default.log` and `build-sim.log` each contain a successful PlatformIO environment result and `EXIT=0`. They identify the respective environments, pinned platform/toolchain packages and captured `already-patched` hook result. Their outputs are full logs (399 lines each), not merely summary excerpts.

The released out-of-tree `platformio.ini` has SHA-256 `bab3cfd8aee5e2b3bede3b527dd4419bf10f0bec5424d67d3c7679d9a83a4199`; it differs from the immutable live `firmware/platformio.ini` only by the isolated evidence-root `build_dir` line. The named target/environment, platform, board, library dependency, hook, source filters, and build flags are otherwise the same. The default environment is `sigap_n16r8`; `sigap_n16r8_sensor_sim` inherits it and adds `-DSIGAP_SENSOR_SIMULATION=1`, so simulation remains opt-in and is not the default. Build logs report default RAM 49,280/327,680 and flash 1,133,453/4,194,304; SIM RAM 50,964/327,680 and flash 1,310,993/4,194,304. These linker figures do not establish runtime heap headroom or task high-water.

Staged artifacts were independently hashed:

| Environment/artifact | Bytes | SHA-256 |
|---|---:|---|
| `sigap_n16r8-firmware.bin` | 1,133,824 | `f759d710b0f3b26d55afe11505cbb64c40191b070dbd381857faaf917449ea7a` |
| `sigap_n16r8-firmware.elf` | 25,013,856 | `3c9df3442b99cf4e93b978b9e9cf4e12298d66c75cc76909d361fc382035698e` |
| `sigap_n16r8_sensor_sim-firmware.bin` | 1,311,360 | `1bb9abe691e9770ee59f7508762c3fc188e11dfac912e6fbe186a04e42b8f72d` |
| `sigap_n16r8_sensor_sim-firmware.elf` | 27,184,212 | `2421c2ee20f2c5f4f336b78240422ab0ce10b54aaf79e73c137da4ceaffa7fba` |

These are retained build candidates, not flash/readback identities. The staged config and logs bind their environment and inputs, though the log files do not preserve the full original shell command line as a separate invocation receipt.

## Dependency hook and remaining acceptance

Read-only checks of the exact `.pio/libdeps/{sigap_n16r8,sigap_n16r8_sensor_sim}/NimBLE-Arduino/src` hook targets confirm, for both environments:

- `nimble/nimble/host/store/config/src/ble_store_nvs.c`: `b5f7d79dcbdc568977f1f93dfe12f7edd046ce832f2eeab8056e4ad81b366bd3` (expected reviewed patched source).
- `NimBLEServer.cpp`: `af4335cf6b9e5ca3be63770307736fe50d29b53529c0dd3579f7ed515b553895` (expected original, non-diagnostic source).

Both build logs say `already-patched` and `repeat-pairing diagnostic patch=not-applied`; current target hashes agree. This is consistent with the hook's no-write path and the correct non-diagnostic server file. No SDK edits were made during this review.

**Outstanding gates:** the required three-transport behavioral regression is NOT independently verified here and remains INCOMPLETE; focused native network checks are NOT_RUN per prior evidence; physical stack high-water/heap, panic absence, authenticated TLS/control/upload, ingestion/ACK/reclaim/web, flash/readback, and device acceptance are NOT_RUN. The 16 KiB setting is a conservative budget, not a measured safe bound. Do not call the whole task PASS or physical validation complete until the assigned regression and remaining separately authorized gates are handled.

## Review artifact

Created only this file: `build-review/REPORT.md`. No product source/configuration, build artifact, test, device, SDK, or prior report was modified.
