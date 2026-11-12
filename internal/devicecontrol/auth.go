package devicecontrol

import (
	"context"
	"crypto/sha256"
	"crypto/subtle"
	"database/sql"
	"encoding/hex"
	"errors"
	"net/http"
	"strings"
	"sync"
	"time"
)

const maxRateLimitKeys = 4096

// DatabaseAuthorizer validates short-lived operator sessions and device
// credentials against the same SQLite database as command persistence. Token
// material is never persisted or returned. A missing session or permission
// row fails closed.
type DatabaseAuthorizer struct {
	db        *sql.DB
	now       func() time.Time
	mu        sync.Mutex
	window    time.Time
	requests  map[string]int
	maxPerMin int
}

func NewDatabaseAuthorizer(db *sql.DB) *DatabaseAuthorizer {
	return &DatabaseAuthorizer{db: db, now: time.Now, requests: make(map[string]int), maxPerMin: 60}
}

func (a *DatabaseAuthorizer) WithClock(now func() time.Time) *DatabaseAuthorizer {
	if now != nil {
		a.now = now
	}
	return a
}

func (a *DatabaseAuthorizer) WithRateLimit(maxPerMinute int) *DatabaseAuthorizer {
	if maxPerMinute > 0 {
		a.maxPerMin = maxPerMinute
	}
	return a
}

func bearer(header string) string {
	parts := strings.Fields(header)
	if len(parts) != 2 || !strings.EqualFold(parts[0], "Bearer") {
		return ""
	}
	return parts[1]
}

func digest(value string) string {
	sum := sha256.Sum256([]byte(value))
	return hex.EncodeToString(sum[:])
}

func (a *DatabaseAuthorizer) allowRate(key string, now time.Time) bool {
	a.mu.Lock()
	defer a.mu.Unlock()
	minute := now.UTC().Truncate(time.Minute)
	if a.window != minute {
		a.window = minute
		clear(a.requests)
	}
	count, tracked := a.requests[key]
	// Reject new keys at capacity without evicting another token's quota.
	if count >= a.maxPerMin || (!tracked && len(a.requests) >= maxRateLimitKeys) {
		return false
	}
	a.requests[key] = count + 1
	return true
}

func (a *DatabaseAuthorizer) AuthorizeOperator(ctx context.Context, r *http.Request, deviceID string) (string, error) {
	if a == nil || a.db == nil || r == nil || strings.TrimSpace(deviceID) == "" {
		return "", ErrUnauthorized
	}
	token := bearer(r.Header.Get("Authorization"))
	if token == "" || len(token) > 512 || !a.allowRate(digest(token), a.now()) {
		return "", ErrUnauthorized
	}
	now := a.now().UTC()
	var operator, expires, revoked, csrfDigest string
	err := a.db.QueryRowContext(ctx, `SELECT operator_id,expires_at,COALESCE(revoked_at,''),csrf_digest FROM device_control_operator_sessions WHERE token_digest=?`, digest(token)).Scan(&operator, &expires, &revoked, &csrfDigest)
	if err != nil || revoked != "" {
		return "", ErrUnauthorized
	}
	exp, err := time.Parse(time.RFC3339Nano, expires)
	if err != nil || !exp.After(now) || operator == "" {
		return "", ErrUnauthorized
	}
	var permissionExpiry, permissionRevoked string
	err = a.db.QueryRowContext(ctx, `SELECT COALESCE(expires_at,''),COALESCE(revoked_at,'') FROM device_control_operator_permissions WHERE operator_id=? AND device_id=?`, operator, deviceID).Scan(&permissionExpiry, &permissionRevoked)
	if err != nil || permissionRevoked != "" {
		return "", ErrUnauthorized
	}
	if permissionExpiry != "" {
		p, parseErr := time.Parse(time.RFC3339Nano, permissionExpiry)
		if parseErr != nil || !p.After(now) {
			return "", ErrUnauthorized
		}
	}
	if r.Method != http.MethodGet && r.Method != http.MethodHead {
		provided := r.Header.Get("X-SIGAP-CSRF")
		if provided == "" || len(provided) > 512 || subtle.ConstantTimeCompare([]byte(digest(provided)), []byte(csrfDigest)) != 1 {
			return "", ErrUnauthorized
		}
	}
	return operator, nil
}

func (a *DatabaseAuthorizer) AuthorizeDevice(ctx context.Context, r *http.Request, deviceID string) error {
	if a == nil || a.db == nil || r == nil || strings.TrimSpace(deviceID) == "" {
		return ErrUnauthorized
	}
	token := bearer(r.Header.Get("Authorization"))
	if token == "" || len(token) > 512 || !a.allowRate("device:"+digest(token), a.now()) {
		return ErrUnauthorized
	}
	var stored string
	if err := a.db.QueryRowContext(ctx, `SELECT credential_digest FROM devices WHERE device_code=?`, deviceID).Scan(&stored); err != nil {
		return ErrUnauthorized
	}
	got := digest(token)
	if subtle.ConstantTimeCompare([]byte(strings.TrimPrefix(stored, "sha256:")), []byte(got)) != 1 {
		return ErrUnauthorized
	}
	return nil
}

// ProvisionOperatorSession is an explicit out-of-band test/administration
// seam. Callers provide disposable token material; only digests are stored.
func ProvisionOperatorSession(ctx context.Context, db *sql.DB, sessionID, operatorID, token, csrf string, expires time.Time, deviceIDs []string) error {
	if db == nil || sessionID == "" || operatorID == "" || token == "" || csrf == "" || expires.IsZero() || len(deviceIDs) == 0 {
		return errors.New("invalid operator session")
	}
	tx, err := db.BeginTx(ctx, nil)
	if err != nil {
		return err
	}
	defer tx.Rollback()
	now := time.Now().UTC().Format(time.RFC3339Nano)
	if _, err = tx.ExecContext(ctx, `INSERT INTO device_control_operator_sessions(session_id,operator_id,token_digest,csrf_digest,expires_at,created_at) VALUES(?,?,?,?,?,?)`, sessionID, operatorID, digest(token), digest(csrf), expires.UTC().Format(time.RFC3339Nano), now); err != nil {
		return err
	}
	for _, deviceID := range deviceIDs {
		if _, err = tx.ExecContext(ctx, `INSERT INTO device_control_operator_permissions(operator_id,device_id) VALUES(?,?)`, operatorID, deviceID); err != nil {
			return err
		}
	}
	return tx.Commit()
}

func RevokeOperatorSession(ctx context.Context, db *sql.DB, sessionID string, now time.Time) error {
	if db == nil || sessionID == "" {
		return ErrUnauthorized
	}
	_, err := db.ExecContext(ctx, `UPDATE device_control_operator_sessions SET revoked_at=? WHERE session_id=?`, now.UTC().Format(time.RFC3339Nano), sessionID)
	return err
}
