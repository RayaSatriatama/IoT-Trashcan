# Canonical record fixture

The JSON file is the complete record object with `record_hash` omitted. Its content
after the repository line ending is removed is the RFC 8785 JCS canonical byte string.
The adjacent digest is the independent expected
`record_hash` value. Implementations must not calculate the expected value from the
implementation under test.
