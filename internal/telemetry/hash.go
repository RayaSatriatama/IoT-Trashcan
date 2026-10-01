package telemetry

import (
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"

	"github.com/lattice-substrate/json-canon/jcs"
	"github.com/lattice-substrate/json-canon/jcstoken"
)

func prefixedSHA256(bytes []byte) string {
	digest := sha256.Sum256(bytes)
	return "sha256:" + hex.EncodeToString(digest[:])
}

func BodyHash(body []byte) string { return prefixedSHA256(body) }

func strictOptions() *jcstoken.Options {
	return &jcstoken.Options{
		MaxDepth:         MaxJSONDepth,
		MaxInputSize:     MaxBodyBytes,
		MaxValues:        MaxRecords * 64,
		MaxObjectMembers: 128,
		MaxArrayElements: MaxRecords,
		MaxStringBytes:   MaxFreeStringBytes,
		MaxNumberChars:   128,
	}
}

func canonicalizeObjectWithoutHash(raw []byte) ([]byte, error) {
	var object map[string]json.RawMessage
	if err := json.Unmarshal(raw, &object); err != nil {
		return nil, contractError(MalformedJSON, "", err.Error())
	}
	if _, ok := object["record_hash"]; !ok {
		return nil, contractError(HashMismatch, "record_hash", "record_hash is required")
	}
	delete(object, "record_hash")
	withoutHash, err := json.Marshal(object)
	if err != nil {
		return nil, contractError(MalformedJSON, "", err.Error())
	}
	canonical, err := jcs.Canonicalize(withoutHash)
	if err != nil {
		return nil, contractError(MalformedJSON, "", err.Error())
	}
	return canonical, nil
}

// RecordHash canonicalizes the complete object after removing only record_hash.
func RecordHash(raw []byte) (string, []byte, error) {
	canonical, err := canonicalizeObjectWithoutHash(raw)
	if err != nil {
		return "", nil, err
	}
	return prefixedSHA256(canonical), canonical, nil
}
