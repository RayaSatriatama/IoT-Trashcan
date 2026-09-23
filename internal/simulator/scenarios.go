package simulator

import (
	"math"
	"math/rand"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

type Phase string

const (
	PhaseNormal      Phase = "NORMAL"
	PhaseWaspada     Phase = "WASPADA"
	PhaseAngkat      Phase = "ANGKAT"
	PhaseSafetyAlert Phase = "SAFETY_ALERT"
	PhaseDataInvalid Phase = "DATA_INVALID"
	PhaseOffline     Phase = "OFFLINE"
	PhaseRecovery    Phase = "RECOVERY"
)

const cycleLength uint64 = 10

func phaseForTick(tick uint64) Phase {
	switch tick % cycleLength {
	case 1:
		return PhaseWaspada
	case 2:
		return PhaseAngkat
	case 3:
		return PhaseSafetyAlert
	case 4:
		return PhaseDataInvalid
	case 5, 6, 7, 8:
		return PhaseOffline
	case 9:
		return PhaseRecovery
	default:
		return PhaseNormal
	}
}

func payloadFor(device Device, phase Phase, recordedAt string, noise *rand.Rand) (ingest.Request, bool) {
	if phase == PhaseOffline && device.ID == "sim-device-lab" {
		return ingest.Request{}, false
	}
	organic := 25.0 + sensorNoise(noise)
	inorganic := 20.0 + sensorNoise(noise)
	b3 := 10.0 + sensorNoise(noise)
	gas := float64(12) + sensorNoise(noise)
	if device.ID == "sim-device-canteen" && canteenWarningPhase(phase) {
		organic = 55 + sensorNoise(noise)
	}
	if device.ID == "sim-device-canteen" && canteenPickupPhase(phase) {
		organic = 90 + sensorNoise(noise)
	}
	if device.ID == "sim-device-lab" && phase == PhaseSafetyAlert {
		gas = 99
	}
	if device.ID == "sim-device-lab" && phase == PhaseDataInvalid {
		inorganic = 110
	}
	organicWeight, inorganicWeight, b3Weight := organic/5, inorganic/5, b3/5
	organicOGI := clamp(25+organic*0.65, 0, 100)
	inorganicOGI := clamp(18+inorganic*0.25, 0, 100)
	b3OGI := clamp(15+gas*0.35, 0, 100)
	accumulation := accumulationForPhase(phase)
	organicState := ingest.SensorValid
	if phase == PhaseRecovery && device.ID == "sim-device-canteen" {
		organicState = ingest.SensorWarmingUp
	}
	inorganicState := ingest.SensorValid
	if phase == PhaseDataInvalid && device.ID == "sim-device-lab" {
		inorganicState = ingest.SensorInvalid
	}
	return ingest.Request{
		DeviceID:   device.Code,
		RecordedAt: recordedAt,
		Health:     []byte(`{"battery":96,"simulated":true}`),
		Slots: []ingest.Slot{
			{Stream: "organic", Fill: floatPointer(organic), Weight: &organicWeight, OGI: &organicOGI, AccumulationHours: &accumulation, Sensors: organicSensors(recordedAt, organic, organicWeight, organicOGI, organicState)},
			{Stream: "inorganic", Fill: floatPointer(inorganic), Weight: &inorganicWeight, OGI: &inorganicOGI, AccumulationHours: &accumulation, Sensors: inorganicSensors(recordedAt, inorganic, inorganicWeight, inorganicOGI, inorganicState)},
			{Stream: "b3", Fill: floatPointer(b3), Weight: &b3Weight, GasRaw: floatPointer(gas), Temperature: floatPointer(27), OGI: &b3OGI, AccumulationHours: &accumulation, Sensors: b3Sensors(recordedAt, b3, b3Weight, b3OGI, gas)},
		},
	}, true
}

func canteenWarningPhase(phase Phase) bool {
	switch phase {
	case PhaseWaspada, PhaseAngkat, PhaseSafetyAlert, PhaseDataInvalid, PhaseOffline:
		return true
	default:
		return false
	}
}

func canteenPickupPhase(phase Phase) bool {
	switch phase {
	case PhaseAngkat, PhaseSafetyAlert, PhaseDataInvalid, PhaseOffline:
		return true
	default:
		return false
	}
}

func sensorNoise(random *rand.Rand) float64 {
	if random == nil {
		return 0
	}
	return float64(random.Intn(5)) - 2
}

func floatPointer(value float64) *float64 { return &value }

func organicSensors(recordedAt string, fill, weight, ogi float64, gasState ingest.SensorState) []ingest.SensorReading {
	return []ingest.SensorReading{
		sensor("organic-fill", "fill", "fill", 100-fill, fill, fill/100, "cm", "%", ingest.SensorValid, recordedAt),
		sensor("organic-hx711", "hx711", "weight", weight*1000, weight, fill/100, "count", "kg", ingest.SensorValid, recordedAt),
		sensor("organic-mq135", "mq135", "broad_gas", 500+ogi*10, math.NaN(), ogi/100, "ADC", "", gasState, recordedAt),
		sensor("organic-mq4", "mq4", "ch4", 400+ogi*8, math.NaN(), ogi/125, "ADC", "", gasState, recordedAt),
	}
}

func inorganicSensors(recordedAt string, fill, weight, ogi float64, state ingest.SensorState) []ingest.SensorReading {
	return []ingest.SensorReading{
		sensor("inorganic-fill", "fill", "fill", 100-fill, fill, fill/100, "cm", "%", state, recordedAt),
		sensor("inorganic-hx711", "hx711", "weight", weight*1000, weight, fill/100, "count", "kg", ingest.SensorValid, recordedAt),
		sensor("inorganic-mq135", "mq135", "broad_gas", 500+ogi*10, math.NaN(), ogi/100, "ADC", "", ingest.SensorValid, recordedAt),
	}
}

func b3Sensors(recordedAt string, fill, weight, ogi, gas float64) []ingest.SensorReading {
	return []ingest.SensorReading{
		sensor("b3-fill", "fill", "fill", 100-fill, fill, fill/100, "cm", "%", ingest.SensorValid, recordedAt),
		sensor("b3-hx711", "hx711", "weight", weight*1000, weight, fill/100, "count", "kg", ingest.SensorValid, recordedAt),
		sensor("b3-mq135", "mq135", "broad_gas", 500+ogi*10, math.NaN(), ogi/100, "ADC", "", ingest.SensorValid, recordedAt),
		sensor("b3-mq7", "mq7", "co", 420+gas*9, math.NaN(), gas/100, "ADC", "", ingest.SensorValid, recordedAt),
		sensor("b3-mq8", "mq8", "h2", 380+gas*4, math.NaN(), gas/200, "ADC", "", ingest.SensorValid, recordedAt),
		sensor("b3-ds18b20", "ds18b20", "temperature", 27, 27, 0.27, "°C", "°C", ingest.SensorValid, recordedAt),
	}
}

func sensor(id, sensorType, parameter string, raw, engineering, normalized float64, rawUnit, unit string, state ingest.SensorState, recordedAt string) ingest.SensorReading {
	reading := ingest.SensorReading{SensorID: id, SensorType: sensorType, Parameter: parameter, RawValue: floatPointer(raw), NormalizedValue: floatPointer(normalized), RawUnit: rawUnit, Unit: unit, SensorState: state, RecordedAt: recordedAt}
	if !math.IsNaN(engineering) {
		reading.EngineeringValue = floatPointer(engineering)
	}
	return reading
}

func accumulationForPhase(phase Phase) float64 {
	switch phase {
	case PhaseWaspada:
		return 9
	case PhaseAngkat, PhaseSafetyAlert, PhaseDataInvalid, PhaseOffline:
		return 13
	case PhaseRecovery:
		return 2
	default:
		return 4
	}
}

func clamp(value, minimum, maximum float64) float64 {
	return math.Max(minimum, math.Min(maximum, value))
}
