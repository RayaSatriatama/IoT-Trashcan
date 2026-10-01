package telemetry

import "bytes"

// PendingBatch freezes exact body bytes at the first send boundary.
// It intentionally has no HTTP, journal, or storage dependency.
type PendingBatch struct {
	BatchID     string
	body        []byte
	bodyHash    string
	attempted   bool
	recordCount uint16
}

func NewPendingBatch(batchID string, body []byte, recordCount uint16) (*PendingBatch, error) {
	if batchID == "" || len(body) > MaxBodyBytes || recordCount > MaxRecords {
		return nil, contractError(BodyTooLarge, "", "invalid pending batch boundary")
	}
	return &PendingBatch{BatchID: batchID, body: append([]byte(nil), body...), recordCount: recordCount}, nil
}

// FreezeForFirstSend computes identity once. Later calls cannot replace the body.
func (p *PendingBatch) FreezeForFirstSend() error {
	if p == nil {
		return contractError(MalformedJSON, "", "nil pending batch")
	}
	if p.attempted {
		return nil
	}
	p.bodyHash = BodyHash(p.body)
	p.attempted = true
	return nil
}

func (p PendingBatch) Attempted() bool     { return p.attempted }
func (p PendingBatch) BodyHash() string    { return p.bodyHash }
func (p PendingBatch) RecordCount() uint16 { return p.recordCount }
func (p PendingBatch) Body() []byte        { return append([]byte(nil), p.body...) }

// RetryBody is intentionally byte-identical, including whitespace.
func (p PendingBatch) RetryBody() []byte {
	if !p.attempted {
		return nil
	}
	return p.Body()
}

func (p PendingBatch) BodyEquals(body []byte) bool { return bytes.Equal(p.body, body) }
