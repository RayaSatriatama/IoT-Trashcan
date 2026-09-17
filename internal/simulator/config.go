package simulator

import (
	"errors"
	"fmt"
	"os"
	"strconv"
	"strings"
	"time"
)

const (
	defaultSeed     int64 = 42
	defaultInterval       = 5 * time.Second
)

var ErrInvalidConfig = errors.New("invalid simulation configuration")

// Config controls the opt-in local simulator. It has no effect when Enabled
// is false and is never used as production decision configuration.
type Config struct {
	Enabled  bool
	Interval time.Duration
	Seed     int64
	Clock    func() time.Time
}

// ParseEnvironment reads only simulation variables. The lookup function is
// injectable so parsing remains deterministic and does not mutate process env.
func ParseEnvironment() (Config, error) {
	return ParseConfig(os.LookupEnv)
}

func ParseConfig(lookup func(string) (string, bool)) (Config, error) {
	enabledText, present := lookup("SIGAP_SIMULATION_ENABLED")
	if !present || strings.TrimSpace(enabledText) == "" || strings.EqualFold(strings.TrimSpace(enabledText), "false") {
		return Config{Seed: defaultSeed}, nil
	}
	if !strings.EqualFold(strings.TrimSpace(enabledText), "true") {
		return Config{}, fmt.Errorf("%w: SIGAP_SIMULATION_ENABLED must be true or false", ErrInvalidConfig)
	}

	interval := defaultInterval
	if value, ok := lookup("SIGAP_SIMULATION_INTERVAL_SECONDS"); ok && strings.TrimSpace(value) != "" {
		seconds, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)
		if err != nil || seconds <= 0 {
			return Config{}, fmt.Errorf("%w: SIGAP_SIMULATION_INTERVAL_SECONDS must be a positive integer", ErrInvalidConfig)
		}
		interval = time.Duration(seconds) * time.Second
	}

	seed := defaultSeed
	if value, ok := lookup("SIGAP_SIMULATION_SEED"); ok && strings.TrimSpace(value) != "" {
		parsed, err := strconv.ParseInt(strings.TrimSpace(value), 10, 64)
		if err != nil {
			return Config{}, fmt.Errorf("%w: SIGAP_SIMULATION_SEED must be an integer", ErrInvalidConfig)
		}
		seed = parsed
	}
	return Config{Enabled: true, Interval: interval, Seed: seed, Clock: time.Now}, nil
}
