package fuzzy

import (
	"fmt"
	"math"
)

type mamdaniEngine struct{ config Config }

// NewEngine validates and retains the supplied rule configuration. No default
// configuration exists because numeric domain thresholds require evidence.
func NewEngine(config Config) (Engine, error) {
	if err := validateConfig(config); err != nil {
		return nil, err
	}
	return &mamdaniEngine{config: config}, nil
}

func (e *mamdaniEngine) Version() string { return e.config.Version }

func (e *mamdaniEngine) Evaluate(input Input) (Result, error) {
	if e == nil {
		return Result{}, fmt.Errorf("%w: engine is nil", ErrInvalidConfig)
	}

	memberships := make(map[string]map[string]float64, len(e.config.Inputs))
	for variableName, variable := range e.config.Inputs {
		value, exists := input[variableName]
		if !exists || !finite(value) {
			return Result{}, fmt.Errorf("%w: %s", ErrMissingInput, variableName)
		}
		degrees := make(map[string]float64, len(variable.Sets))
		for setName, membership := range variable.Sets {
			degrees[setName] = boundedDegree(membership.Degree(value))
		}
		memberships[variableName] = degrees
	}

	results := make([]RuleResult, 0, len(e.config.Rules))
	outputs := make(map[string][]Membership)
	var dominant *RuleResult
	for _, rule := range e.config.Rules {
		strength, err := e.ruleStrength(rule, input)
		if err != nil {
			return Result{}, err
		}
		ruleResult := RuleResult{ID: rule.ID, Strength: strength, Output: rule.Output, Reason: rule.Reason}
		results = append(results, ruleResult)
		if strength == 0 {
			continue
		}
		output := e.config.Outputs[rule.Output]
		outputs[rule.Output] = append(outputs[rule.Output], clippedMembership{base: output.Membership, limit: strength})
		if dominant == nil || strength > dominant.Strength {
			selected := ruleResult
			dominant = &selected
		}
	}
	if dominant == nil {
		return Result{}, ErrNoRuleFired
	}

	aggregated := make(map[string]Membership, len(outputs))
	allSets := make([]Membership, 0, len(outputs))
	for output, sets := range outputs {
		set := maximumMembership{sets: sets}
		aggregated[output] = set
		allSets = append(allSets, set)
	}
	score, err := centroid(maximumMembership{sets: allSets}, e.config.OutputRange)
	if err != nil {
		return Result{}, err
	}
	return Result{
		Score:            score,
		Status:           e.config.Outputs[dominant.Output].Status,
		DominantReason:   dominant.Reason,
		Version:          e.config.Version,
		Rules:            results,
		InputMemberships: memberships,
		Aggregated:       aggregated,
	}, nil
}

func (e *mamdaniEngine) ruleStrength(rule Rule, input Input) (float64, error) {
	strength := 1.0
	for _, condition := range rule.Conditions {
		value, exists := input[condition.Variable]
		if !exists {
			return 0, fmt.Errorf("%w: %s", ErrMissingInput, condition.Variable)
		}
		membership := e.config.Inputs[condition.Variable].Sets[condition.Set]
		strength = math.Min(strength, boundedDegree(membership.Degree(value)))
	}
	return strength, nil
}

func centroid(set Membership, output Range) (float64, error) {
	var numerator, denominator float64
	for x := output.Min; ; {
		degree := boundedDegree(set.Degree(x))
		numerator += x * degree
		denominator += degree

		if x >= output.Max {
			break
		}
		next := x + output.Step
		if next <= x || next > output.Max {
			break
		}
		x = next
	}
	if denominator == 0 {
		return 0, ErrNoRuleFired
	}
	return numerator / denominator, nil
}

func boundedDegree(value float64) float64 {
	if math.IsNaN(value) || value <= 0 {
		return 0
	}
	if value >= 1 {
		return 1
	}
	return value
}

func validateConfig(config Config) error {
	if config.Version == "" || len(config.Inputs) == 0 || len(config.Outputs) == 0 || len(config.Rules) == 0 {
		return ErrInvalidConfig
	}
	if !finite(config.OutputRange.Min) || !finite(config.OutputRange.Max) || !finite(config.OutputRange.Step) || config.OutputRange.Step <= 0 || config.OutputRange.Max <= config.OutputRange.Min {
		return fmt.Errorf("%w: output range", ErrInvalidConfig)
	}
	for name, variable := range config.Inputs {
		if name == "" || len(variable.Sets) == 0 {
			return fmt.Errorf("%w: input %q", ErrInvalidConfig, name)
		}
		for setName, set := range variable.Sets {
			if setName == "" || set == nil {
				return fmt.Errorf("%w: input set %q", ErrInvalidConfig, setName)
			}
		}
	}
	for name, output := range config.Outputs {
		if name == "" || output.Status == "" || output.Membership == nil {
			return fmt.Errorf("%w: output %q", ErrInvalidConfig, name)
		}
	}
	for _, rule := range config.Rules {
		if rule.ID == "" || rule.Output == "" || rule.Reason == "" || len(rule.Conditions) == 0 {
			return fmt.Errorf("%w: rule", ErrInvalidConfig)
		}
		if _, exists := config.Outputs[rule.Output]; !exists {
			return fmt.Errorf("%w: rule output %q", ErrInvalidConfig, rule.Output)
		}
		for _, condition := range rule.Conditions {
			variable, exists := config.Inputs[condition.Variable]
			if !exists || condition.Set == "" || variable.Sets[condition.Set] == nil {
				return fmt.Errorf("%w: condition %s/%s", ErrInvalidConfig, condition.Variable, condition.Set)
			}
		}
	}
	return nil
}

func finite(value float64) bool {
	return !math.IsNaN(value) && !math.IsInf(value, 0)
}
