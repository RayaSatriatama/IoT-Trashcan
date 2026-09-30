package httpapi

import (
	"encoding/csv"
	"encoding/json"
	"net/http"
	"net/http/httptest"
	"net/url"
	"reflect"
	"strconv"
	"strings"
	"testing"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/history"
)

func TestReadAPIHistorySupportsCombinedFiltersAndNulls(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	request := httptest.NewRequest(http.MethodGet, "/api/history?from=2026-09-17T09:59:40Z&to=2026-09-17T09:59:43Z&location_id=location-a&device_id=device-a&status=Safety%20Alert", nil)
	response := httptest.NewRecorder()

	api.History(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if strings.Contains(response.Body.String(), "credential_digest") || strings.Contains(response.Body.String(), "fixture-secret") {
		t.Fatalf("history response leaked secret: %s", response.Body.String())
	}
	var body struct {
		Items []struct {
			ReadingID         int64    `json:"reading_id"`
			Stream            string   `json:"stream"`
			GasRaw            *float64 `json:"gas_raw"`
			OGI               *float64 `json:"ogi"`
			AccumulationHours *float64 `json:"accumulation_hours"`
			Status            *string  `json:"fis_status"`
			Safety            *string  `json:"safety_state"`
		} `json:"items"`
		Filters struct {
			LocationID *string `json:"location_id"`
			Status     *string `json:"status"`
		} `json:"filters"`
		Page struct {
			Limit      int  `json:"limit"`
			HasMore    bool `json:"has_more"`
			TotalCount int  `json:"total_count"`
		} `json:"page"`
		Summary history.Summary `json:"summary"`
		Trend   history.Trend   `json:"trend"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if len(body.Items) != 1 || body.Items[0].ReadingID != 3 || body.Items[0].Stream != "b3" {
		t.Fatalf("history items=%+v", body.Items)
	}
	if body.Items[0].GasRaw != nil || body.Items[0].OGI != nil || body.Items[0].AccumulationHours != nil {
		t.Fatalf("history values=%+v", body.Items[0])
	}
	if body.Items[0].Status == nil || *body.Items[0].Status != "Angkat" || body.Items[0].Safety == nil || *body.Items[0].Safety != "Safety Alert" {
		t.Fatalf("decision values=%+v", body.Items[0])
	}
	if body.Filters.LocationID == nil || *body.Filters.LocationID != "location-a" || body.Filters.Status == nil || *body.Filters.Status != "Safety Alert" {
		t.Fatalf("filter echo=%+v", body.Filters)
	}
	if body.Page.Limit != 50 || body.Page.HasMore || body.Page.TotalCount != 1 {
		t.Fatalf("page=%+v", body.Page)
	}
	if body.Summary.Total != body.Page.TotalCount || body.Summary.SafetyAlert != 1 || body.Summary.Pickup != 0 {
		t.Fatalf("summary=%+v page=%+v", body.Summary, body.Page)
	}
	if body.Trend.DefaultWindow || len(body.Trend.Buckets) != 1 || body.Trend.Buckets[0].SafetyAlert != 1 {
		t.Fatalf("filtered trend=%+v", body.Trend)
	}
}

func TestReadAPIHistoryRejectsInvalidFilters(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	for _, path := range []string{
		"/api/history?from=not-a-date",
		"/api/history?from=2026-09-18T00:00:00Z&to=2026-09-17T00:00:00Z",
		"/api/history?status=unknown",
		"/api/history?status=Offline",
		"/api/history?limit=0",
		"/api/history?limit=201",
		"/api/history?before=2026-09-17T09:59:41Z",
		"/api/history?before=not-a-date&before_id=2",
		"/api/history?before=2026-09-17T09:59:41Z&before_id=0",
	} {
		request := httptest.NewRequest(http.MethodGet, path, nil)
		response := httptest.NewRecorder()
		api.History(response, request)
		if response.Code != http.StatusBadRequest {
			t.Errorf("%s status=%d body=%s", path, response.Code, response.Body.String())
		}
	}
}

func TestReadAPIHistoryAcceptsDeviceCodeSearch(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	request := httptest.NewRequest(http.MethodGet, "/api/history?device_id=SIGAP-A&limit=10", nil)
	response := httptest.NewRecorder()

	api.History(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	var body struct {
		Items []struct {
			DeviceCode string `json:"device_code"`
		} `json:"items"`
		Page struct {
			TotalCount int `json:"total_count"`
		} `json:"page"`
		Summary history.Summary `json:"summary"`
		Trend   history.Trend   `json:"trend"`
	}
	if err := json.Unmarshal(response.Body.Bytes(), &body); err != nil {
		t.Fatal(err)
	}
	if len(body.Items) != 3 || body.Page.TotalCount != 3 || body.Items[0].DeviceCode != "SIGAP-A" {
		t.Fatalf("device code filter=%+v", body)
	}
	if body.Summary.Total != body.Page.TotalCount || !body.Trend.DefaultWindow || len(body.Trend.Buckets) != 30 {
		t.Fatalf("analytics=%+v page=%+v", body, body.Page)
	}
}

func TestReadAPIHistoryUsesStableCursorPagination(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	firstRequest := httptest.NewRequest(http.MethodGet, "/api/history?limit=2", nil)
	firstResponse := httptest.NewRecorder()

	api.History(firstResponse, firstRequest)

	if firstResponse.Code != http.StatusOK {
		t.Fatalf("first status=%d body=%s", firstResponse.Code, firstResponse.Body.String())
	}
	var first struct {
		Items []struct {
			ReadingID int64  `json:"reading_id"`
			Recorded  string `json:"recorded_at"`
		} `json:"items"`
		Page struct {
			Limit      int  `json:"limit"`
			HasMore    bool `json:"has_more"`
			TotalCount int  `json:"total_count"`
			Next       *struct {
				Recorded string `json:"recorded_at"`
				ID       int64  `json:"reading_id"`
			} `json:"next"`
		} `json:"page"`
		Summary history.Summary `json:"summary"`
		Trend   history.Trend   `json:"trend"`
	}
	if err := json.Unmarshal(firstResponse.Body.Bytes(), &first); err != nil {
		t.Fatal(err)
	}
	if len(first.Items) != 2 || first.Items[0].ReadingID != 3 || first.Items[1].ReadingID != 2 {
		t.Fatalf("first items=%+v", first.Items)
	}
	if first.Page.Limit != 2 || !first.Page.HasMore || first.Page.TotalCount != 4 || first.Page.Next == nil || first.Page.Next.ID != 2 {
		t.Fatalf("first page=%+v", first.Page)
	}
	if first.Summary.Total != first.Page.TotalCount || len(first.Trend.Buckets) != 30 || !first.Trend.DefaultWindow {
		t.Fatalf("first analytics summary=%+v trend=%+v", first.Summary, first.Trend)
	}

	secondPath := "/api/history?limit=2&before=" + url.QueryEscape(first.Page.Next.Recorded) + "&before_id=" + strconv.FormatInt(first.Page.Next.ID, 10)
	secondRequest := httptest.NewRequest(http.MethodGet, secondPath, nil)
	secondResponse := httptest.NewRecorder()
	api.History(secondResponse, secondRequest)
	if secondResponse.Code != http.StatusOK {
		t.Fatalf("second status=%d body=%s", secondResponse.Code, secondResponse.Body.String())
	}
	var second struct {
		Items []struct {
			ReadingID int64 `json:"reading_id"`
		} `json:"items"`
		Page struct {
			HasMore    bool `json:"has_more"`
			TotalCount int  `json:"total_count"`
		} `json:"page"`
		Summary history.Summary `json:"summary"`
		Trend   history.Trend   `json:"trend"`
	}
	if err := json.Unmarshal(secondResponse.Body.Bytes(), &second); err != nil {
		t.Fatal(err)
	}
	if len(second.Items) != 2 || second.Items[0].ReadingID != 1 || second.Items[1].ReadingID != 4 || second.Page.HasMore || second.Page.TotalCount != 4 {
		t.Fatalf("second page items=%+v page=%+v", second.Items, second.Page)
	}
	if second.Summary.Total != second.Page.TotalCount {
		t.Fatalf("second summary=%+v page=%+v", second.Summary, second.Page)
	}
	if reflect.DeepEqual(first.Items, second.Items) || !reflect.DeepEqual(first.Summary, second.Summary) || !reflect.DeepEqual(first.Trend, second.Trend) {
		t.Fatalf("cursor analytics changed first=%+v second=%+v", first, second)
	}
}

func TestReadAPIExportCSVUsesHistoryProjectionAndFilters(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	// The B3 fixture has underlying FIS=Angkat but an effective Safety Alert.
	// Export must use the same effective-status filter semantics as History.
	request := httptest.NewRequest(http.MethodGet, "/api/export.csv?device_id=device-a&status=Safety%20Alert", nil)
	response := httptest.NewRecorder()

	api.ExportCSV(response, request)

	if response.Code != http.StatusOK {
		t.Fatalf("status=%d body=%s", response.Code, response.Body.String())
	}
	if got := response.Header().Get("Content-Type"); got != "text/csv; charset=utf-8" {
		t.Fatalf("Content-Type=%q", got)
	}
	if got := response.Header().Get("Content-Disposition"); got != "attachment; filename=\"sigap-export.csv\"" {
		t.Fatalf("Content-Disposition=%q", got)
	}
	if strings.Contains(response.Body.String(), "credential_digest") || strings.Contains(response.Body.String(), "fixture-secret") || strings.Contains(response.Body.String(), "ppm") {
		t.Fatalf("CSV contains forbidden content: %s", response.Body.String())
	}
	rows, err := csv.NewReader(strings.NewReader(response.Body.String())).ReadAll()
	if err != nil {
		t.Fatal(err)
	}
	if len(rows) != 2 || rows[0][0] != "recorded_at" || rows[1][7] != "b3" || rows[1][11] != "" {
		t.Fatalf("CSV rows=%+v", rows)
	}
	if rows[1][12] != "" || rows[1][13] != "" {
		t.Fatalf("missing normalized values should be empty: %+v", rows[1])
	}
}

func TestReadAPIExportCSVRejectsInvalidFilter(t *testing.T) {
	api, db := readAPIFixture(t)
	defer db.Close()
	for _, path := range []string{
		"/api/export.csv?status=unknown",
		"/api/export.csv?status=Offline",
	} {
		request := httptest.NewRequest(http.MethodGet, path, nil)
		response := httptest.NewRecorder()
		api.ExportCSV(response, request)
		if response.Code != http.StatusBadRequest {
			t.Fatalf("%s status=%d body=%s", path, response.Code, response.Body.String())
		}
	}
}
