package ingest

import (
	"crypto/sha256"
	"crypto/subtle"
	"encoding/hex"
)

func credentialMatches(token, storedHash string) bool {
	if token == "" {
		return false
	}

	expected, err := hex.DecodeString(storedHash)
	if err != nil || len(expected) != sha256.Size {
		return false
	}
	actual := sha256.Sum256([]byte(token))
	return subtle.ConstantTimeCompare(actual[:], expected) == 1
}
