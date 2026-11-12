# Task 9 Playwright coverage

This suite exercises the accepted web UI in a real Chromium browser. It is read-only: browser requests are restricted to the isolated loopback origin and GET/HEAD/OPTIONS; the Bluetooth chooser is disabled. It does not establish hardware, firmware, calibration, or physical HIL success.

## Run

Start a disposable SIGAP runtime on `127.0.0.1:18772` using a fresh SQLite snapshot (never the source database), with simulation disabled and the operator origin set to `http://127.0.0.1:18772`. Then supply all three explicit values:

```sh
SIGAP_E2E_ORIGIN=http://127.0.0.1:18772 \
SIGAP_E2E_OUTPUT=/absolute/path/to/isolated-artifacts \
SIGAP_E2E_BROWSER=/usr/bin/chromium \
npm run test:e2e
```

The output path must be a dedicated absolute directory outside the project checkout: filesystem root and checkout paths are rejected, and the runner requires a fresh or empty output directory before dispatch. Worker config loads permit that validated directory after Playwright begins creating results. Use a fresh child per run (or create an empty dedicated directory); config loading never creates or clears it. JSON/HTML reports and failed-test traces are written under the selected output directory. There are no retries, skipped tests, fixed sleeps, or forced clicks.

The suite uses desktop 1920×1080 and mobile 390×844 contexts, checks the 710/711 and 425/426 responsive boundaries, and covers device tools, sensor selection, Wi-Fi/debug panels, calibration wizard visibility and safe closure, keyboard focus/blur and Escape, genuine touch open/toggle/reopen with opacity and viewport-bound assertions, attached tooltip screenshots, layout containment, touch targets, actual hint-control reduced-motion transitions, and browser runtime errors.

The recorded completion run targets the parent-owned corrected embedded runtime at `http://127.0.0.1:18772`; the tests do not rebuild or modify product/backend source and do not establish physical HIL success.
