package httpapi

import (
	"errors"
	"net/http"
	"strconv"
	"strings"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/history"
)

type historyPageState struct {
	Limit   int             `json:"limit"`
	HasMore bool            `json:"has_more"`
	Next    *history.Cursor `json:"next"`
}

type historyResponse struct {
	Items   []history.Record    `json:"items"`
	Filters history.FilterState `json:"filters"`
	Page    historyPageState    `json:"page"`
}

func parseHistoryFilters(r *http.Request) (history.Filters, error) {
	filters, err := history.ParseFilters(r.URL.Query())
	if err != nil {
		return history.Filters{}, err
	}
	// Offline is derived from the current device heartbeat, not persisted as a
	// historical reading/decision state. Reject it until historical offline
	// semantics are explicitly defined rather than returning a misleading empty set.
	if filters.Status == string(device.StatusOffline) {
		return history.Filters{}, history.ErrInvalidFilter
	}
	return filters, nil
}

func parseHistoryPage(r *http.Request) (history.PageOptions, error) {
	values := r.URL.Query()
	limit := history.DefaultPageSize
	if raw := strings.TrimSpace(values.Get("limit")); raw != "" {
		parsed, err := strconv.Atoi(raw)
		if err != nil || parsed < 1 || parsed > history.MaxPageSize {
			return history.PageOptions{}, history.ErrInvalidFilter
		}
		limit = parsed
	}

	before := strings.TrimSpace(values.Get("before"))
	beforeID := strings.TrimSpace(values.Get("before_id"))
	if (before == "") != (beforeID == "") {
		return history.PageOptions{}, history.ErrInvalidFilter
	}
	if before == "" {
		return history.PageOptions{Limit: limit}, nil
	}
	if _, err := time.Parse(time.RFC3339Nano, before); err != nil {
		return history.PageOptions{}, history.ErrInvalidFilter
	}
	readingID, err := strconv.ParseInt(beforeID, 10, 64)
	if err != nil || readingID <= 0 {
		return history.PageOptions{}, history.ErrInvalidFilter
	}
	return history.PageOptions{
		Limit: limit,
		Before: &history.Cursor{
			RecordedAt: before,
			ReadingID:  readingID,
		},
	}, nil
}

// History returns a bounded audit page from readings and decisions. Cursor
// pagination keeps interactive History responsive while new readings arrive.
func (api *ReadAPI) History(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	filters, err := parseHistoryFilters(r)
	if err != nil {
		if errors.Is(err, history.ErrInvalidFilter) || errors.Is(errors.Unwrap(err), history.ErrInvalidFilter) {
			writeError(w, http.StatusBadRequest, "invalid history filter")
			return
		}
		writeError(w, http.StatusBadRequest, "invalid history filter")
		return
	}
	page, err := parseHistoryPage(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid history pagination")
		return
	}
	result, err := history.QueryPage(r.Context(), api.db, filters, page)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, historyResponse{
		Items:   result.Items,
		Filters: filters.State(),
		Page: historyPageState{
			Limit:   page.Limit,
			HasMore: result.HasMore,
			Next:    result.Next,
		},
	})
}

// ExportCSV writes the complete history projection and applies exactly the
// same domain filters as GET /api/history. Pagination parameters are not used
// for export so a filtered CSV is never silently truncated.
func (api *ReadAPI) ExportCSV(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	filters, err := parseHistoryFilters(r)
	if err != nil {
		writeError(w, http.StatusBadRequest, "invalid history filter")
		return
	}
	items, err := history.Query(r.Context(), api.db, filters)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	w.Header().Set("Content-Type", "text/csv; charset=utf-8")
	w.Header().Set("Content-Disposition", "attachment; filename=\"sigap-export.csv\"")
	if err := history.WriteCSV(w, items); err != nil {
		return
	}
}
