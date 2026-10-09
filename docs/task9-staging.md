# Task 9 staging backend procedure

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
