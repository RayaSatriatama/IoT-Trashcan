package simulator

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"math/rand"
	"strings"
	"sync"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/fuzzy"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

const demoFISVersion = "simulation-organic-fis-v2"

type Simulator struct {
	service  *ingest.Service
	dataset  Dataset
	config   Config
	now      func() time.Time
	random   *rand.Rand
	logger   *log.Logger
	mu       sync.Mutex
	tick     uint64
	lastTime time.Time
}

func New(service *ingest.Service, dataset Dataset, config Config) *Simulator {
	now := config.Clock
	if now == nil {
		now = time.Now
	}
	return &Simulator{
		service: service,
		dataset: dataset,
		config:  config,
		now:     now,
		random:  rand.New(rand.NewSource(config.Seed)),
		logger:  log.Default(),
	}
}

// Run executes sequential ticks and exits when the application context is
// cancelled. No second tick can start while the current tick is running.
func (s *Simulator) Run(ctx context.Context) error {
	if s == nil || s.service == nil {
		return errors.New("simulation service is unavailable")
	}
	if s.config.Interval <= 0 {
		return fmt.Errorf("%w: interval must be positive", ErrInvalidConfig)
	}
	if err := s.Tick(ctx); err != nil {
		s.logTickError(err)
	}
	ticker := time.NewTicker(s.config.Interval)
	defer ticker.Stop()
	for {
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-ticker.C:
			if err := s.Tick(ctx); err != nil {
				s.logTickError(err)
			}
		}
	}
}

func (s *Simulator) Tick(ctx context.Context) error {
	s.mu.Lock()
	defer s.mu.Unlock()
	phase := phaseForTick(s.tick)
	recordedAt := s.nextTimestamp()
	s.tick++
	var firstErr error
	for _, simulated := range s.dataset.Devices {
		request, send := payloadFor(simulated, phase, recordedAt, s.random)
		if !send {
			continue
		}
		rawPayload, err := json.Marshal(request)
		if err != nil {
			if firstErr == nil {
				firstErr = err
			}
			s.logger.Printf("simulation device=%s phase=%s error=encode_payload", simulated.ID, phase)
			continue
		}
		if _, err := s.service.Ingest(ctx, simulated.Token, request, rawPayload); err != nil {
			if firstErr == nil {
				firstErr = err
			}
			s.logger.Printf("simulation device=%s phase=%s error=%s", simulated.ID, phase, errorCategory(err))
		}
	}
	return firstErr
}

func (s *Simulator) nextTimestamp() string {
	now := s.now().UTC()
	if !s.lastTime.IsZero() && !now.After(s.lastTime) {
		now = s.lastTime.Add(time.Nanosecond)
	}
	s.lastTime = now
	return now.Format(time.RFC3339Nano)
}

func (s *Simulator) logTickError(err error) {
	if err != nil && !errors.Is(err, context.Canceled) {
		s.logger.Printf("simulation tick error=%s", errorCategory(err))
	}
}

func errorCategory(err error) string {
	message := strings.ToLower(err.Error())
	switch {
	case strings.Contains(message, "unauthorized"):
		return "unauthorized"
	case strings.Contains(message, "invalid"):
		return "invalid_payload"
	case strings.Contains(message, "persist"):
		return "persistence"
	default:
		return "runtime"
	}
}

// DemoEngine is deliberately isolated under simulator and must never be used
// as a production default. Its thresholds are presentation fixtures only.
func DemoEngine() fuzzy.Engine {
	return DemoEngines()["organic"]
}

// DemoEngines returns simulation-only stream profiles. These thresholds are
// fixtures for UI and integration testing, never production defaults.
func DemoEngines() map[string]fuzzy.Engine {
	return map[string]fuzzy.Engine{
		"organic":   mustDemoEngine("simulation-organic-fis-v2", true),
		"inorganic": mustDemoEngine("simulation-inorganic-fis-v2", false),
		"b3":        mustDemoEngine("simulation-b3-fis-v2", false),
	}
}

func mustDemoEngine(version string, gasCanRequirePickup bool) fuzzy.Engine {
	inputSets := func() map[string]fuzzy.Membership {
		return map[string]fuzzy.Membership{
			"low":    fuzzy.TrapezoidMembership(0, 0, 30, 45),
			"medium": fuzzy.TrapezoidMembership(30, 45, 60, 75),
			"high":   fuzzy.TrapezoidMembership(60, 75, 100, 100),
		}
	}
	timeSets := map[string]fuzzy.Membership{
		"low":    fuzzy.TrapezoidMembership(0, 0, 4, 7),
		"medium": fuzzy.TrapezoidMembership(4, 7, 12, 16),
		"high":   fuzzy.TrapezoidMembership(12, 16, 24, 24),
	}
	gasHighOutput := "warning"
	gasHighReason := "simulation gas index needs attention"
	if gasCanRequirePickup {
		gasHighOutput = "pickup"
		gasHighReason = "simulation organic gas index is high"
	}
	rules := []fuzzy.Rule{
		{ID: "fill-low", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "low"}}, Output: "normal", Reason: "simulation fill remains low"},
		{ID: "fill-medium", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "medium"}}, Output: "warning", Reason: "simulation fill needs attention"},
		{ID: "fill-high", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "high"}}, Output: "pickup", Reason: "simulation fill requires pickup"},
		{ID: "weight-low", Conditions: []fuzzy.Condition{{Variable: "weight_utilization", Set: "low"}}, Output: "normal", Reason: "simulation weight remains low"},
		{ID: "weight-medium", Conditions: []fuzzy.Condition{{Variable: "weight_utilization", Set: "medium"}}, Output: "warning", Reason: "simulation weight needs attention"},
		{ID: "weight-high", Conditions: []fuzzy.Condition{{Variable: "weight_utilization", Set: "high"}}, Output: "pickup", Reason: "simulation weight requires pickup"},
		{ID: "ogi-low", Conditions: []fuzzy.Condition{{Variable: "ogi", Set: "low"}}, Output: "normal", Reason: "simulation gas index remains low"},
		{ID: "ogi-medium", Conditions: []fuzzy.Condition{{Variable: "ogi", Set: "medium"}}, Output: "warning", Reason: "simulation gas index needs attention"},
		{ID: "ogi-high", Conditions: []fuzzy.Condition{{Variable: "ogi", Set: "high"}}, Output: gasHighOutput, Reason: gasHighReason},
		{ID: "time-low", Conditions: []fuzzy.Condition{{Variable: "accumulation_hours", Set: "low"}}, Output: "normal", Reason: "simulation accumulation time remains low"},
		{ID: "time-medium", Conditions: []fuzzy.Condition{{Variable: "accumulation_hours", Set: "medium"}}, Output: "warning", Reason: "simulation accumulation time needs attention"},
		{ID: "time-high", Conditions: []fuzzy.Condition{{Variable: "accumulation_hours", Set: "high"}}, Output: "warning", Reason: "simulation accumulation time is high"},
	}
	engine, err := fuzzy.NewEngine(fuzzy.Config{
		Version: version,
		Inputs: map[string]fuzzy.InputVariable{
			"fill":               {Sets: inputSets()},
			"weight_utilization": {Sets: inputSets()},
			"ogi":                {Sets: inputSets()},
			"accumulation_hours": {Sets: timeSets},
		},
		Outputs: map[string]fuzzy.OutputSet{
			"normal":  {Status: "Normal", Membership: fuzzy.TriangleMembership(0, 10, 30)},
			"warning": {Status: "Waspada", Membership: fuzzy.TriangleMembership(25, 50, 70)},
			"pickup":  {Status: "Angkat", Membership: fuzzy.TriangleMembership(65, 90, 100)},
		},
		Rules:       rules,
		OutputRange: fuzzy.Range{Min: 0, Max: 100, Step: 1},
	})
	if err != nil {
		panic(err)
	}
	return engine
}

func DemoSafetyConfig() safety.Config {
	return safety.Config{Thresholds: []safety.Threshold{
		{Signal: safety.SignalMQ7CO, AtOrAbove: 0.8, Reason: "simulation-only MQ-7 CO index alert"},
		{Signal: safety.SignalMQ8H2, AtOrAbove: 0.8, Reason: "simulation-only MQ-8 H2 index alert"},
		{Signal: safety.SignalDS18B20Temperature, AtOrAbove: 55, Reason: "simulation-only DS18B20 temperature alert"},
	}}
}
