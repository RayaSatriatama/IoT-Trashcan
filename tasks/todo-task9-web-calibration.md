# Task 9 web calibration checklist

Current source `839bffa` includes the bounded firmware-side HTTPS poller
composition and the integrated operator/device-control routes. Physical radio,
staging endpoint, and operator-provider execution remain separate gates.

- [x] Update PRD/TSD and acceptance matrix with WEB-CAL-01..18.
- [x] Define shared bounded calibration command/result contract.
- [x] Implement firmware controller and BLE adapter with native tests.
- [x] Implement BLE calibration framing/client/server tests.
- [x] Implement persistent Go command queue and authenticated APIs.
- [x] Implement website diagnostics, wizard, capabilities, and telemetry boundary.
- [x] Run focused and integrated verification.
- [x] Update customization and verification reports.
- [x] Keep PR #27 open; do not start Task 10.
- [x] Wire and verify the firmware-side nonblocking HTTPS command poller.
- [ ] Inject the production operator authorization provider and run approved HIL.
