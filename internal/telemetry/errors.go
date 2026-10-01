package telemetry

import "fmt"

// ErrorCode is the stable machine-readable v3 contract failure taxonomy.
type ErrorCode string

const (
	MalformedJSON     ErrorCode = "MALFORMED_JSON"
	DuplicateKey      ErrorCode = "DUPLICATE_KEY"
	BodyTooLarge      ErrorCode = "BODY_TOO_LARGE"
	RecordTooLarge    ErrorCode = "RECORD_TOO_LARGE"
	DepthExceeded     ErrorCode = "DEPTH_EXCEEDED"
	StringTooLong     ErrorCode = "STRING_TOO_LONG"
	SchemaUnsupported ErrorCode = "SCHEMA_UNSUPPORTED"
	InvalidUint64     ErrorCode = "INVALID_UINT64"
	InvalidIdentity   ErrorCode = "INVALID_IDENTITY"
	InvalidEnum       ErrorCode = "INVALID_ENUM"
	CountMismatch     ErrorCode = "COUNT_MISMATCH"
	HashMismatch      ErrorCode = "HASH_MISMATCH"
	DuplicateRecordID ErrorCode = "DUPLICATE_RECORD_ID"
	InvalidUTF8       ErrorCode = "INVALID_UTF8"
	NonFiniteNumber   ErrorCode = "NONFINITE_NUMBER"
	UnknownField      ErrorCode = "UNKNOWN_FIELD"
	ProfileMalformed  ErrorCode = "PROFILE_REF_INVALID"
	SourceIDsMissing  ErrorCode = "SOURCE_SAMPLE_IDS_MISSING"
	Conflict          ErrorCode = "CONFLICT"
	AckMismatch       ErrorCode = "ACK_MISMATCH"
)

// ContractError is returned for all boundary validation failures.
type ContractError struct {
	Code ErrorCode
	Path string
	Msg  string
}

func (e *ContractError) Error() string {
	if e.Path == "" {
		return fmt.Sprintf("%s: %s", e.Code, e.Msg)
	}
	return fmt.Sprintf("%s at %s: %s", e.Code, e.Path, e.Msg)
}

func contractError(code ErrorCode, path, msg string) error {
	return &ContractError{Code: code, Path: path, Msg: msg}
}
