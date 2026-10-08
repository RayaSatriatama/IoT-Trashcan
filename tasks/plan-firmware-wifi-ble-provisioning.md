# Task 7 implementation plan: Wi-Fi and always-on BLE provisioning

## Scope

Remediate the existing network transport foundation and add always-on,
device-local BLE provisioning with browser Web Bluetooth support. HTTPS archive
delivery remains a transport foundation and is not treated as the BLE gateway.

## Shared contracts

- Wi-Fi association is an instance-owned state machine with bounded scan and
  connect deadlines.
- A candidate credential stays in RAM until association and IP acquisition
  succeed, then is committed through `SecretStore`; failed candidates preserve
  the previous credential.
- BLE framing is bounded, versioned, and session-scoped. GATT characteristics
  require an encrypted link; plaintext secrets never appear in status/export.
- Web Bluetooth is initiated only from a user gesture and only in a secure
  context. Password bytes go directly to the BLE write characteristic.
- Network HTTP work is asynchronous relative to the application loop.

## Work packages

1. Network transport: asynchronous execution, persistent authorization failure,
   strict ACK parsing, bounded delayed response handling, Wi-Fi reconnect, and
   jitter. Native tests cover each failure path.
2. Firmware provisioning: Wi-Fi manager, BLE protocol/session, target GATT
   service, SecretStore candidate flow, and native protocol tests.
3. Web provisioning: device detail dialog, Web Bluetooth client, privacy and
   browser-support tests, typecheck, and web export.
4. Integration: app ownership, policy/documentation, normative PRD/TSD update,
   resource audit, full firmware/backend/schema verification, and review gate.

## Checkpoints

- [x] Network package tests and target compile pass.
- [x] BLE/Wi-Fi package tests and target compile pass.
- [x] Web tests, typecheck, and export pass.
- [x] Integrated native, sanitizer, cppcheck, four target builds, Go/schema,
      and frozen hash checks pass.
- [x] PR #25 review blockers are closed; independent review before merge.

## Deferred

No sensor, actuator, heater, credential, SD, reboot, or physical commissioning
evidence is claimed in this task without explicit authorization.
