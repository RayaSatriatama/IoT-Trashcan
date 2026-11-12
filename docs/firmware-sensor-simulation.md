# Firmware sensor simulation: configuration and operator guide

## Purpose and limits

The firmware build can select deterministic, software-generated sensor-reader inputs while retaining the ordinary acquisition graph, processing, archive and telemetry path. This is not the separate Go demo simulator and is not a hardware test. Simulated values do not establish wiring, sensor operation, calibration, gas concentration, electrical safety, or physical acceptance.

## Build selection and safe return to physical mode

`SIGAP_SENSOR_SIMULATION` is a compile-time numeric option in `firmware/include/sigap/build/build_config.h`: it defaults to `0` and rejects values other than `0` or `1`. The normal PlatformIO environment does not set it, so its default is OFF. The current source does not provide a runtime toggle or switchable setting. Changing mode requires building and installing a firmware image with the desired compile-time value; it is not changed by a web control, reboot alone, or saved device configuration.

Simulation is selected in the acquisition composition using compile-time reader types. In the simulation branch the selected readers (including the heater rail) are non-actuating software implementations; target GPIO/I2C reader constructors are not selected. The normal OFF branch selects the existing target readers. The test harness checks constructor-time IO separation, but this is host evidence, not a board observation. A simulation-tagged image should be identified by its BOOT firmware version `sigap-task6-sensor-sim-v1`; normal mode uses `sigap-task6`. That tag is producer-declared build metadata, not hardware attestation. Confirm the exact installed image through an authorized procedure before relying on either mode.

**Operator rule:** do not treat the build option as a live safety interlock. Before returning a board to service, build and install the approved normal/OFF image, then verify its identity and complete the applicable commissioning checks. This guide does not authorize flashing, reset, configuration/profile writes, heater use, or other hardware actions.

The `firmware/platformio.ini` default environment remains `sigap_n16r8` (OFF). The explicit opt-in environment is `sigap_n16r8_sensor_sim`; it inherits the normal target and adds `-DSIGAP_SENSOR_SIMULATION=1`. From `firmware/`, build with `pio run -e sigap_n16r8_sensor_sim`; build the physical/OFF image with `pio run -e sigap_n16r8`. Both target configurations have compiled successfully, and the simulation ELF contains the expected BOOT build ID. No upload command is provided here. Build success does not authorize installation or establish hardware acceptance.

## Sensor inventory, pins, streams and cadence

These are the source-defined N16R8 pin assignments (`firmware/include/sigap/board/board_pins.h`) and factory sampling defaults (`firmware/include/sigap/config/config_defaults.h`). Active persisted sampling settings may differ. Channels are the firmware's 13 sensor identities (8 sensor families); the two SHT31 devices each provide temperature and relative humidity.

| Sensor identity | Stream | Physical interface pins | Factory sampling default | Raw / reported units |
|---|---|---|---:|---|
| `hx711_organic` | Organic | DOUT 10, SCK 11 | 100 ms | Signed 24-bit counts; converted kg only with a matching valid profile |
| `hx711_inorganic` | Inorganic | DOUT 12, SCK 13 | 100 ms | Signed 24-bit counts; converted kg only with a matching valid profile |
| `hx711_b3` | B3 | DOUT 14, SCK 15 | 100 ms | Signed 24-bit counts; converted kg only with a matching valid profile |
| `ultrasonic_organic` | Organic | TRIG 16, ECHO 17 | 1000 ms | Echo pulse microseconds; converted distance requires profile parameters |
| `ultrasonic_inorganic` | Inorganic | TRIG 18, ECHO 21 | 1000 ms | Echo pulse microseconds; converted distance requires profile parameters |
| `ultrasonic_b3` | B3 | TRIG 47, ECHO 42 | 1000 ms | Echo pulse microseconds; converted distance requires profile parameters |
| `mq4` | Organic | ADC GPIO 4 | 100 ms | ADC counts; derived voltage only with a matching valid ADC profile; not ppm |
| `mq135` | Organic | ADC GPIO 5 | 100 ms | ADC counts; derived voltage only with a matching valid ADC profile; not ppm |
| `mq7` | B3 | ADC GPIO 6; heater-select GPIO 1 in physical mode | 100 ms | ADC counts; derived voltage only with a matching valid ADC profile; not ppm |
| `mq8` | B3 | ADC GPIO 7 | 100 ms | ADC counts; derived voltage only with a matching valid ADC profile; not ppm |
| `ds18b20_b3` | B3 | 1-Wire data GPIO 2 | 1000 ms | Temperature in centi-°C internally / °C when converted; resolution 9–12 bit has 94/188/375/750 ms conversion deadlines |
| `sht31_44` | Organic | I²C SDA 8, SCL 9; address `0x44` | 2000 ms | Temperature centi-°C and RH centi-percent internally; displayed as °C and %RH |
| `sht31_45` | B3 | I²C SDA 8, SCL 9; address `0x45` | 2000 ms | Temperature centi-°C and RH centi-percent internally; displayed as °C and %RH |

The ultrasonic transducer model is not specified here; do not infer a model from the interface. ADC resolution/attenuation is not established by this sensor map. The MQ-7 heater is a physical actuator in normal mode and is unavailable in the simulated reader path; do not use simulation to claim heater readiness.

## Interpretation and profile safety

- Read raw values, converted values, quality, profile references, and origin as separate facts. Missing, invalid, warming, timeout, CRC-error, uncalibrated, and test-only states must not be relabeled as a good physical measurement.
- A physical profile may be present while simulated readers are selected. Converted engineering values still depend on that profile; this does not make the simulated input or profile physically verified. Do not interpret simulated HX values as operational load/capacity, ultrasonic values as actual fill distance, or MQ readings as concentration.
- The simulation app supplies a RAM-only DS18B20 protocol fixture with `TestOnly` validity. It generates the ROM and scratchpad bytes, including their CRCs and resolution field, and passes them through the normal parser and archive. The explicit byte-availability flag means complete protocol bytes exist; it is not physical bus capture or hardware attestation.
- SHT31 raw `temperature_ticks` and `humidity_ticks` are the original unsigned 16-bit words from each address's frame. Converted centi-units are separate. Legacy archived samples lack these exact ticks and remain unavailable; the batch builder rejects required evidence that is missing rather than reconstructing it.
- MQ-7 raw includes the actual controller phase (`PREHEAT`, `HIGH_CLEAN`, `LOW_DETECT`, or `FAULT`). A simulated heater rail remains unavailable and non-actuating; its `FAULT` state is not a heater-readiness claim.
- Simulation mode blocks calibration and sensor-profile mutations at the shared maintenance router and rejects controller mutations before replay-store lookup or workflow dispatch. Initialized SIM/OFF host controls exercise actual USB maintenance/shared workflow, BLE backend/controller, and the device-command codec plus application HTTP executor. The latter is not an HTTPS socket, TLS, or poller test. Valid OFF START/CANCEL controls distinguish policy rejection from an uninitialized identity. SIM checks compare encoded active/draft configuration, the nonempty RAM-only DS profile, profile generation, audit size, workflow fields, mock NVS contents and callback/write counts; a populated read-only replay entry must survive denied operations. Repeating a failed request must not promote it to successful replay. These are scoped host assertions, not proof of every RAM byte, every profile-import workflow, or physical acceptance.

## Reading the MCU sensor pane

The device-detail MCU pane is a manual, read-only diagnostics view. It requests the canonical 13 sensor IDs once per button activation through the existing operator-authorized diagnostics API; it does not start a periodic poll or issue calibration commands. Enter only the approved short-lived operator authorization in the masked field. It is cleared on submission and device-context change; do not paste it into logs or screenshots. A missing sensor response is shown as unavailable; failed requests should be retried only after resolving the authorization or connection issue. Leaving the device view aborts in-flight requests. Browser component lifecycle tests are controlled hook/effect tests, not physical BLE or browser acceptance.

Interpret origin labels literally: “Simulasi sensor firmware” means the matching telemetry sample was classified from firmware BOOT metadata; “Fisik (metadata firmware)” is still a firmware-declared label, not independent hardware proof; “Asal tidak diketahui” is not physical. Quality and profile metadata remain relevant even when values are present.

## Verification boundary

The version-1 archive frame header, identity, CRC and commit marker are unchanged. Current samples carry a tagged/versioned optional payload extension for MQ-7 phase and original SHT31 ticks. Tests cover a synthetic current-base payload with that extension removed; this is a documented pre-extension layout fixture, not captured hardware data or proof of every older payload layout. Missing original evidence is never reconstructed.

The LittleFS pending selector retains unsupported, validly framed records from a different nonzero boot ID of the same device in their original archive. It tests eligibility with the unchanged telemetry-v3 builder. Eligible records can progress through normal ACK/checkpoint/reclaim while retained originals remain undelivered and count against quota. Invalid boot identity, another device, or a current-boot producer fault is not silently classified as historical. The SD adapter and frozen batch/journal codecs are unchanged; this is not a conversion or guarantee of delivery of unsupported records on either medium.

Traversal inspects at most two frames per selector invocation, at most 32 KiB of frame reads. Selection and eligibility run under the app's existing filesystem lease. A false pending peek can mean an exhausted selection slice or retained unsupported data, not an empty archive. Drain checks must still require zero pending bytes and empty acquisition/processing handoffs and pending batch. Retention counters describe only the inspected prefix since recovery, not a full-archive census. The one-shot `LEGACY_PENDING_RETAINED=DEGRADED` runtime marker explicitly reports `originals=UNDELIVERED`; a restarted selector discovers retention again without persisting a new sidecar or modifying originals. Existing cached unsupported manifest bodies are not rewritten or promoted to delivered by selection.

Host probes cover all 13 acquisition identities through the current archive codec and telemetry-v3 `BatchJsonBuilder`, including generated DS bytes and the FAULT-phase MQ-7 batch. A separate acquisition test checks all four MQ-7 controller phases; SHT frame-decoder controls check tick extrema, while both-address acquisition/codec/builder checks preserve the original ticks and converted centi-units. Separate regressions check DS converted value/null and canonical extension flags. These boundaries must not be combined into a claim that every phase/extreme was authenticated end-to-end. The scoped SIM admission assertions described above are host evidence, not physical acceptance.

### End-to-end integration status

Historical failure: the corrected candidate was executed four times, exceeding the owner's three-attempt bound: approximately 16:28, 16:30, 16:33 and 16:59 on 2026-10-29. This overrun is retained, not reset or relabeled. That run accepted nine batches, then failed `application archive/handoff not drained after actual ACKs`. Exact requests/ACKs remain under `/home/raya/.hermes/artifacts/sigap-total-repair-20261029-01/firmware/actual-roundtrip-01/`; attempt history is in `resume-2016/RECOVERY.txt` under the same artifact root.

The first stalled frame was a RESULT with a malformed profile-reference layout. Its writer now includes the required presence byte. Component tests cover complete profile/source identities, DS raw evidence with converted value/explicit null, and strict availability flags. The eight-ACK oracle incorrectly assumed allocation sequence was enqueue order and failed three current-candidate invocations. After explicit owner renewal, the corrected nine-exact-ACK check passed. The stronger full-app host oracle subsequently passed with 167 batches, 9 processing results and 13 sensor identities, with archive pending bytes, acquisition and processing handoffs, and pending batch asserted empty. Evidence is `/home/raya/.hermes/artifacts/sigap-final-proof-20261029-2349/drain-oracle-verification/`; that run is not legacy-selector coverage and does not assert every storage-worker internal queue.

The stronger host result predates the legacy-selector change; source-bound post-change integration and independent review are separate gates. The SIM image installed on 2026-10-30 has exact readback SHA-256 `6251f8c82b948c01fcb1e233ebc79704a7b6b61f79c92f6a273d1fc104faf600` and does not contain this later selector. Flash/readback is not boot, TLS or browser proof. Actual device-to-web delivery and Wi-Fi/BLE/storage HIL remain separate from host simulation; no physical sensor/heater/actuator acceptance is claimed. The normal SIM target still needs approved out-of-tree endpoint/CA configuration. Fresh scoped owner renewal is recorded in `resume-20261030-1744/AUTHORIZATION.txt` under the proof root; it permits bounded additional host integration and installed-image boot/browser checks, not extra flashing or deployment.
