// Package fuzzy provides a configurable Mamdani inference engine. It does not
// embed SIGAP production thresholds: callers must supply an evidence-backed
// configuration.
package fuzzy

import "errors"

var (
	ErrInvalidConfig = errors.New("invalid fuzzy configuration")
	ErrMissingInput  = errors.New("missing fuzzy input")
	ErrNoRuleFired   = errors.New("no fuzzy rule fired")
)

// Membership describes a fuzzy set over a single numeric dimension.
type Membership interface {
	Degree(x float64) float64
}

// Input names are deliberately domain-neutral. The decision pipeline decides
// which normalized SIGAP values may be supplied to a particular Config.
type Input map[string]float64

type InputVariable struct {
	Sets map[string]Membership
}

type OutputSet struct {
	Status     string
	Membership Membership
}

type Condition struct {
	Variable string
	Set      string
}

type Rule struct {
	ID         string
	Conditions []Condition
	Output     string
	Reason     string
}

type Range struct {
	Min  float64
	Max  float64
	Step float64
}

// Config is intentionally required; the package exposes no production rules
// or numeric thresholds.
type Config struct {
	Version     string
	Inputs      map[string]InputVariable
	Outputs     map[string]OutputSet
	Rules       []Rule
	OutputRange Range
}

type RuleResult struct {
	ID       string
	Strength float64
	Output   string
	Reason   string
}

type Result struct {
	Score          float64
	Status         string
	DominantReason string
	Version        string
	Rules          []RuleResult
	// Aggregated exposes the output membership after max aggregation for
	// callers that need an auditable calculation trace.
	Aggregated map[string]Membership
}

type Engine interface {
	Evaluate(Input) (Result, error)
	Version() string
}
