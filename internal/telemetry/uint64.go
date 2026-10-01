package telemetry

import (
	"strconv"
	"strings"
)

// ParseUint64Decimal accepts only the canonical decimal wire representation.
func ParseUint64Decimal(value string) (uint64, error) {
	if value == "" || (len(value) > 1 && value[0] == '0') {
		return 0, contractError(InvalidUint64, "", "decimal string is empty or has a leading zero")
	}
	for _, char := range value {
		if char < '0' || char > '9' {
			return 0, contractError(InvalidUint64, "", "decimal string contains a non-digit")
		}
	}
	parsed, err := strconv.ParseUint(value, 10, 64)
	if err != nil {
		return 0, contractError(InvalidUint64, "", "decimal string does not fit uint64")
	}
	return parsed, nil
}

// SequenceSortKey creates the future SQLite ordering key without converting to a signed integer.
func SequenceSortKey(value string) (string, error) {
	parsed, err := ParseUint64Decimal(value)
	if err != nil {
		return "", err
	}
	canonical := strconv.FormatUint(parsed, 10)
	return strings.Repeat("0", 20-len(canonical)) + canonical, nil
}
