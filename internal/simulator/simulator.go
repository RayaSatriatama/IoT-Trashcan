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

const demoFISVersion = "simulation-only-fis-v1"

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
	engine, err := fuzzy.NewEngine(fuzzy.Config{
		Version: demoFISVersion,
		Inputs: map[string]fuzzy.InputVariable{
			"fill": {Sets: map[string]fuzzy.Membership{
				"low":    fuzzy.TrapezoidMembership(0, 0, 35, 50),
				"medium": fuzzy.TrapezoidMembership(35, 45, 60, 70),
				"high":   fuzzy.TrapezoidMembership(55, 75, 100, 100),
			}},
		},
		Outputs: map[string]fuzzy.OutputSet{
			"normal":  {Status: "Normal", Membership: fuzzy.TriangleMembership(0, 10, 30)},
			"warning": {Status: "Waspada", Membership: fuzzy.TriangleMembership(25, 50, 70)},
			"pickup":  {Status: "Angkat", Membership: fuzzy.TriangleMembership(65, 90, 100)},
		},
		Rules: []fuzzy.Rule{
			{ID: "sim-fill-low", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "low"}}, Output: "normal", Reason: "simulation-only fill remains low"},
			{ID: "sim-fill-medium", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "medium"}}, Output: "warning", Reason: "simulation-only fill needs attention"},
			{ID: "sim-fill-high", Conditions: []fuzzy.Condition{{Variable: "fill", Set: "high"}}, Output: "pickup", Reason: "simulation-only fill requires pickup"},
		},
		OutputRange: fuzzy.Range{Min: 0, Max: 100, Step: 1},
	})
	if err != nil {
		panic(err)
	}
	return engine
}

func DemoSafetyConfig() safety.Config {
	return safety.Config{Thresholds: []safety.Threshold{{Signal: safety.SignalGasRaw, AtOrAbove: 50, Reason: "simulation-only B3 gas fixture"}}}
}
