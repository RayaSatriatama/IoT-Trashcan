package fuzzy

import "math"

type triangle struct{ a, b, c float64 }

// TriangleMembership creates a triangle. Equal adjacent values intentionally
// represent a shoulder, allowing a configuration to express open-ended sets.
func TriangleMembership(a, b, c float64) Membership {
	return triangle{a: a, b: b, c: c}
}

func (m triangle) Degree(x float64) float64 {
	if m.a > m.b || m.b > m.c || math.IsNaN(x) {
		return 0
	}
	if m.a == m.b && m.b == m.c {
		if x == m.a {
			return 1
		}
		return 0
	}
	if m.a == m.b {
		if x <= m.b {
			return 1
		}
		if x >= m.c {
			return 0
		}
		return (m.c - x) / (m.c - m.b)
	}
	if m.b == m.c {
		if x <= m.a {
			return 0
		}
		if x >= m.b {
			return 1
		}
		return (x - m.a) / (m.b - m.a)
	}
	if x <= m.a || x >= m.c {
		return 0
	}
	if x == m.b {
		return 1
	}
	if x < m.b {
		return (x - m.a) / (m.b - m.a)
	}
	return (m.c - x) / (m.c - m.b)
}

type trapezoid struct{ a, b, c, d float64 }

// TrapezoidMembership creates a trapezoid. Equal outer pairs represent
// shoulders, such as [a,a,c,d] for a left-shoulder set.
func TrapezoidMembership(a, b, c, d float64) Membership {
	return trapezoid{a: a, b: b, c: c, d: d}
}

func (m trapezoid) Degree(x float64) float64 {
	if m.a > m.b || m.b > m.c || m.c > m.d || math.IsNaN(x) {
		return 0
	}
	if m.a == m.b && m.b == m.c && m.c == m.d {
		if x == m.a {
			return 1
		}
		return 0
	}
	if m.a == m.b && x <= m.b {
		return 1
	}
	if m.c == m.d && x >= m.c {
		return 1
	}
	if x <= m.a || x >= m.d {
		return 0
	}
	if x >= m.b && x <= m.c {
		return 1
	}
	if x < m.b {
		return (x - m.a) / (m.b - m.a)
	}
	return (m.d - x) / (m.d - m.c)
}

type clippedMembership struct {
	base  Membership
	limit float64
}

func (m clippedMembership) Degree(x float64) float64 {
	return math.Min(m.limit, m.base.Degree(x))
}

type maximumMembership struct {
	sets []Membership
}

func (m maximumMembership) Degree(x float64) float64 {
	var maximum float64
	for _, set := range m.sets {
		maximum = math.Max(maximum, set.Degree(x))
	}
	return maximum
}
