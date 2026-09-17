package simulator

import (
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
	return ingest.Request{
		DeviceID:   device.Code,
		RecordedAt: recordedAt,
		Health:     []byte(`{"battery":96,"simulated":true}`),
		Slots: []ingest.Slot{
			{Stream: "organic", Fill: floatPointer(organic), Weight: floatPointer(organic / 5), Temperature: floatPointer(27)},
			{Stream: "inorganic", Fill: floatPointer(inorganic), Weight: floatPointer(inorganic / 5), Temperature: floatPointer(27)},
			{Stream: "b3", Fill: floatPointer(b3), Weight: floatPointer(b3 / 5), GasRaw: floatPointer(gas), Temperature: floatPointer(27)},
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
