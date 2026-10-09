# Task 9 frontend dependency audit

Audit date: 2026-10-09

## Scope and commands

The audit was run from the repository root with the committed lockfile:

```text
npm audit --json
npm audit --omit=dev --json
npm ls shell-quote source-map-js esbuild node-forge vitest
npm test -- --run
npm run typecheck
npm run export:web
```

The audit was not fixed with `npm audit fix` or `--force`. The lockfile uses
three narrow root overrides for patched transitive versions:

| Package | Previous | Locked | Reason |
| --- | ---: | ---: | --- |
| `shell-quote` | 1.10.0 | 1.12.0 | command-injection advisory; pulled by React DevTools |
| `source-map-js` | 1.2.1 | 1.2.2 | indexed source-map denial-of-service advisory; pulled by PostCSS/Metro |
| `esbuild` | 0.27.7 | 0.28.2 | development-server file-read advisory; compatible with the declared Vite and Tamagui ranges |

These overrides do not alter the Expo, React Native, Tamagui, or Vitest major
versions. They were installed with `npm install --ignore-scripts --no-audit`
and verified by the frontend test, typecheck, and web export commands above.

## Results

Before the overrides, `npm audit` reported 41 findings (1 low, 11 moderate,
26 high, 3 critical). The production-filtered report contained 31 findings
(0 low, 10 moderate, 20 high, 1 critical).

After the overrides:

* Full dependency graph: 38 findings (0 low, 11 moderate, 25 high, 2 critical).
* `--omit=dev`: 29 findings (0 low, 10 moderate, 19 high, 0 critical).

The remaining full-graph critical findings are `vitest`/`tinypool`. Vitest is
a development test runner and the available fixed release is 5.0.3, a major
upgrade from the pinned 3.x line. It remains deferred until a dedicated
Vitest compatibility change can be tested. The remaining high findings are
primarily Expo/Metro/React Native toolchain advisories whose automated fixes
request incompatible major releases (Expo 44 or React Native 0.72) for the
current Expo 57/React Native 0.86 application. `node-forge` is used by Expo
CLI code-signing tooling and has no newer fixed release in the current Expo
chain.

The production-filtered report has no critical findings after the overrides.
The unresolved production findings are retained as an explicit upgrade
backlog, not silently suppressed. Review them again when the Expo SDK 57
patch line or a controlled SDK-major migration supplies compatible fixed
versions.

## Runtime relevance and controls

`shell-quote`, Metro, Expo CLI, `source-map-js`, and esbuild are build/dev
tooling paths and are not bundled into the device-facing web application
runtime. `node-forge` is used by Expo CLI signing tooling. The backend and
firmware do not consume this npm dependency graph. No production credential,
WiFi password, token, or provisioning secret is present in the audit output.

No package was removed, no broad upgrade was applied, and no audit finding was
reclassified as fixed solely from a test pass. The exact lockfile remains the
reviewable source of dependency versions.

## Follow-up

Before release, run the audit again with the deployment build and review the
Expo/RN upgrade path. Upgrade Vitest to a compatible fixed major only with a
separate compatibility test. Re-run `npm test`, typecheck, export, and the
full audit after any dependency change.
