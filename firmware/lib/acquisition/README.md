# SIGAP acquisition foundation

This library contains bounded, hardware-independent acquisition seams for Task 2.
Each driver emits a typed sample attempt through `RecordFactory`; raw evidence is
retained when a conversion profile is unavailable or a read fails. The scheduler
uses monotonic time and never waits on a sensor.

The ultrasonic 60 ms inter-sensor gap and the default echo timeout are engineering
inputs. Echo-to-distance conversion is profile-owned. MQ drivers expose ADC data
and optional engineering millivolts only; ppm, R0, OGI, and alarm thresholds are
deliberately absent. MQ-7 phase timings are supplied by `Mq7HeaterProfile` and are
not universal production constants.

No network, storage, telemetry, or calibration maintenance workflow is present in
this slice. `profile_store.h` is only the typed boundary for the later atomic
profile store.
