# Task 9 staging backend procedure

Attached-only SD validation is recorded separately in
`docs/task9-verification.md`. It passed on the final `sigap_n16r8_sd` image
without formatting, erasing, or touching unrelated files. This staging
document still describes loopback HTTPS software evidence; no physical ESP32
HTTPS transmission or production endpoint was used.

This procedure is for a disposable, loopback-only Go backend. It does not
deploy an endpoint and does not contain a certificate, device token, or other
secret. The staging database must be separate from any production database.

## Prerequisites

- Go 1.27.1 (or the repository toolchain) is available.
- A disposable device identity and token are supplied out-of-band.
- A trusted CA and server certificate for a local staging hostname are
  supplied or generated in a user-owned temporary directory.
- The firmware endpoint and CA are configured separately in the authorized
  commissioning build. They are never committed to this repository.

## Fresh loopback calibration staging evidence

The software-only staging path has an executable isolated HTTPS regression.
`internal/e2e/task9_calibration_journey_test.go` opens the same migrated
SQLite schema used by the server, provisions disposable operator and device
digests, starts an ephemeral `httptest.NewTLSServer`, and drives the production
router through START, SET_CRITERIA, CAPTURE, VALIDATE, PREVIEW, and COMMIT.
The synthetic device consumes the actual delivery envelope and posts typed
results through the production result endpoint. Operator readback, missing
CSRF rejection, exact COMMIT replay, and changed-result conflict are asserted.
The profile readback in this test is an explicit synthetic durable profile
seam, not physical ESP32 evidence.

Fresh command:

```text
go test -count=1 ./internal/e2e -run TestTask9RemoteCalibrationLoopbackJourney -v
PASS (6 lifecycle subtests; ephemeral TLS; isolated SQLite; no external network)
```

The test certificate and all credentials exist only in process memory. This
does not prove ESP32 HTTPS reachability, private-LAN hostname routing,
firmware task scheduling, or physical profile persistence. Those remain
separate commissioning gates.

## Loopback launch

Generate or obtain `cert.pem` and `key.pem` outside the repository, then run:

```text
SIGAP_HTTP_ADDR=127.0.0.1:18443 \
SIGAP_DB_PATH=/tmp/sigap-task9-staging.sqlite \
SIGAP_TLS_REQUIRED=true \
SIGAP_TLS_CERT_FILE=/tmp/sigap-task9-tls/cert.pem \
SIGAP_TLS_KEY_FILE=/tmp/sigap-task9-tls/key.pem \
go run ./cmd/sigap
```

When TLS is enabled, the process fails closed unless both certificate and key
are present and regular files. The default development entrypoint remains
HTTP on `:8080`; it must not be used for a commissioning or device test.

Use a local hostname covered by the certificate and verify the CA explicitly.
Do not use `-k`, disable hostname verification, bind to `0.0.0.0`, or expose
the staging process through a public tunnel.

## Evidence and cleanup

Capture only redacted HTTP status, batch identity, ACK status, and database
path. Never print bearer tokens, WiFi passwords, BLE setup secrets, private
keys, or full Authorization headers. Replay uses the exact frozen request
bytes. The staging SQLite file and temporary TLS directory are disposable and
must be removed only after the authorized run has completed and evidence is
preserved without secrets.

The physical ESP32 commissioning, upload, reboot, BLE pairing, WiFi change,
SD write, and HTTPS transmission remain separate owner-authorized operations.
