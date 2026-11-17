# MQ-135 NH3-equivalent research curve (Winsen v1.6, Figure 3)

Status: **digitized research approximation. Not a manufacturer numeric
specification, not a selective NH3 measurement, not a certified calibration and
not a validated physical accuracy claim.** Physical HIL and any ppm accuracy
claim remain **HOLD**.

## Source

| Item | Value |
| --- | --- |
| Manufacturer | Zhengzhou Winsen Electronics Technology Co., Ltd. |
| Document | MQ135 Semiconductor Sensor for Air Quality |
| Version / valid from | 1.6 / 2021-07-01 |
| Owner-spec URL | `https://www.winsen-sensor.com/d/files/mq135-%28ver1_6%29---manual.pdf` (returns HTTP 403 to this environment) |
| Figure used | Figure 3, "Typical Sensitivity Curve" — NH3 curve only (green, third legend entry) |
| Axes | log-log; X = NH3 concentration (ppm), Y = Rs/R0 |
| Archived EN PDF | SHA256 `8f268a7a6e5ae628ecef182ddc14e5f8637bc1fed967c9e5aead90e6d79b6a59`, Wayback `20260721012439` |
| Archived CN PDF | SHA256 `ae6de0be527d163c06af88100cd1629df918dda5351042e2b468e99126974e7f`, Wayback `20260216225043` |
| Figure 3 raster (EN) | SHA256 `99c6158347058e73761b3ca399e35d043d42b5e885964ce1d7ea983a62a9aa55`, 400x305 px, PDF object 50, flate |
| Figure 3 raster (CN) | SHA256 `06ede753fa53a641642db1e1b3efa929ab9dfa3d2555c7d4b5368814215cc872`, 400x305 px, PDF object 42, jpeg |

The plot is a **raster** image inside the PDF, not vector art. Curve identity was
established by binding raster swatch rows to the PDF text-layer label baselines in
page coordinates: legend order top-to-bottom is H2 (blue), Air (red), NH3 (green),
Toluene (purple). The Air line is the only horizontal stroke (Rs/R0 ~ 1.0), which
cross-checks the row binding. Curve identity and geometry were reproduced in a
second independent pass (maximum deviation 1.46 %).

The owner-spec URL could not be fetched live (403); every value below comes from
the archived copies above, whose hashes are recorded in the review artifacts.

## Dataset

`docs/mq135-winsen-nh3-curve.csv`

* SHA256 `4aa984238efa7d253b77ac828babd40e3a8f10ef14a3a86f8b9f1da0e6dd414b`
* column bound by the implementation: **`rs_r0`** (the raw `rs_r0_raw` column is
  **not** used)
* 13 source points, concentration strictly increasing, ratio strictly decreasing
* exact domain: ratio `0.0893 .. 0.6559`, nominal concentration `10 .. 1000` ppm
* `sigma_pct` is the reviewed per-point **digitization** uncertainty (0.70 % interior,
  1.18 %/1.05 % at gridline crossings, 2.00 % at the clipped 10 ppm end bar,
  1.38 % at 1000 ppm). It is **not** a physical concentration uncertainty.

Firmware binding: `firmware/lib/acquisition/src/acquisition/mq135_nh3_curve.h`
carries the same 13 values, the same curve identity (`curve_id`, `curve_version`
2) and the SHA256 above. `test_mq135_nh3_curve` re-hashes this CSV file and
compares every row against the compiled table, so the header cannot drift from
the reviewed source silently.

## Estimator

Piecewise log-log interpolation between adjacent knots only:

```
t     = (ln(r) - ln(r1)) / (ln(r2) - ln(r1))
ln(C) = ln(C1) + t * (ln(C2) - ln(C1))
C     = exp(ln(C))
```

* No extrapolation and **no clamping**. `r > 0.6559` is reported as
  `RatioAboveDomain` (below 10 ppm) and `r < 0.0893` as `RatioBelowDomain`
  (above 1000 ppm); neither produces a ppm value.
* Exact source knots are reproduced exactly instead of through `exp(ln(C))`.
* `r = 0.6559 -> 10.0` and `r = 0.0893 -> 1000.0` are exact domain checks.
  `r = 0.0902` is a valid **interior** ratio and yields ~978.030 ppm — that is
  interpolation, not a clamped 1000 ppm.
* Non-finite or non-positive ratios, unusable tables, duplicated or reordered
  points and tables not carrying the reviewed digest are rejected.
* The pre-existing synthetic log-regression helper in `mq135_nh3.h` is unchanged
  and remains `production_eligible() == false`; so does the new curve/estimator.

## What this is not

* No Figure 4 temperature/humidity (H2 reference) correction is applied.
* No CO2 or third-party MQ-135 constants and no arbitrary `R0` are used.
* MQ-135 is cross-sensitive: a numerically correct estimate is **not** a
  selective NH3 measurement.
* The firmware does not yet publish ppm: `q0` persistence, single-controller
  clean-air capture, the MQG2 producer/consumer contract and the qualified UI
  remain the next dependent slice.
