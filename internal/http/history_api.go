package httpapi

import (
	"errors"
	"net/http"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/history"
)

type historyResponse struct {
	Items   []history.Record   `json:"items"`
	Filters history.FilterState `json:"filters"`
}

// History returns audit rows from readings and decisions using the shared
// filter contract consumed by CSV export.
func (api *ReadAPI) History(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	filters, err := history.ParseFilters(r.URL.Query())
	if err != nil {
		if errors.Is(err, history.ErrInvalidFilter) || errors.Is(errors.Unwrap(err), history.ErrInvalidFilter) {
			writeError(w, http.StatusBadRequest, "invalid history filter")
			return
		}
		writeError(w, http.StatusBadRequest, "invalid history filter")
		return
	}
	items, err := history.Query(r.Context(), api.db, filters)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, historyResponse{Items: items, Filters: filters.State()})
}

// ExportCSV writes the same history projection and applies exactly the same
// filters as GET /api/history.
func (api *ReadAPI) ExportCSV(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	filters, err := history.ParseFilters(r.URL.Query())
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
	w.Header().Set("Content-Disposition", `attachment; filename="sigap-export.csv"`)
	if err := history.WriteCSV(w, items); err != nil {
		return
	}
}
