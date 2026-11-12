package devicecontrol

import "testing"

func TestClassifyDiagnosticOriginRequiresOneRecognizedFirmwareTag(t *testing.T) {
	for _, test := range []struct {
		name string
		tags []string
		want SampleOrigin
	}{
		{"simulated", []string{"sigap-task6-sensor-sim-v1"}, OriginSimulated},
		{"physical", []string{"sigap-task6"}, OriginPhysical},
		{"physical4-v1", []string{"sigap-task6-physical4-v1"}, OriginPhysical},
		{"physical4-v2", []string{"sigap-task6-physical4-v2"}, OriginPhysical},
		{"physical4-near-miss", []string{"sigap-task6-physical4-v3"}, OriginUnknown},
		{"missing", nil, OriginUnknown},
		{"unrecognized", []string{"1.0.0"}, OriginUnknown},
		{"malformed", []string{" "}, OriginUnknown},
		{"conflicting", []string{"sigap-task6", "sigap-task6-sensor-sim-v1"}, OriginUnknown},
		{"duplicate", []string{"sigap-task6", "sigap-task6"}, OriginUnknown},
	} {
		t.Run(test.name, func(t *testing.T) {
			origin, version := classifyDiagnosticOrigin(test.tags)
			if origin != test.want {
				t.Fatalf("origin=%q want %q", origin, test.want)
			}
			if origin == OriginUnknown && version != "" {
				t.Fatalf("unknown origin disclosed version %q", version)
			}
		})
	}
}
