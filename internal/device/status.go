// Package device contains device-level operational state helpers.
package device

// Status is the canonical operational status exposed by SIGAP.
type Status string

const (
	StatusNormal      Status = "Normal"
	StatusWaspada     Status = "Waspada"
	StatusAngkat      Status = "Angkat"
	StatusSafetyAlert Status = "Safety Alert"
	StatusOffline     Status = "Offline"
	StatusDataInvalid Status = "Data Invalid"
)

// Compare reports the operational precedence of a over b. Equal precedence
// does not collapse distinct statuses such as Offline and Data Invalid.
func Compare(a, b Status) int {
	pa, pb := precedence(a), precedence(b)
	switch {
	case pa > pb:
		return 1
	case pa < pb:
		return -1
	default:
		return 0
	}
}

// Aggregate returns the highest-precedence status. When statuses have equal
// precedence, the first one is retained so their distinct meanings are not
// silently rewritten.
func Aggregate(statuses ...Status) Status {
	if len(statuses) == 0 {
		return StatusNormal
	}

	best := statuses[0]
	for _, candidate := range statuses[1:] {
		if Compare(candidate, best) > 0 {
			best = candidate
		}
	}
	return best
}

func precedence(status Status) int {
	switch status {
	case StatusSafetyAlert:
		return 5
	case StatusAngkat:
		return 4
	case StatusWaspada:
		return 3
	case StatusDataInvalid, StatusOffline:
		return 2
	case StatusNormal:
		return 1
	default:
		return 0
	}
}
