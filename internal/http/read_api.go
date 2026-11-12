package httpapi

import (
	"context"
	"database/sql"
	"encoding/json"
	"errors"
	"net/http"
	"strings"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/device"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/safety"
)

// ReadAPI provides the public monitoring views over persisted readings and
// decisions. It intentionally returns DTOs rather than database row structs.
type ReadAPI struct {
	db           *sql.DB
	now          func() time.Time
	offlineAfter time.Duration
}

// ReadAPIOption customizes read-time derivation without inventing production
// defaults for domain inputs such as the offline duration.
type ReadAPIOption func(*ReadAPI)

// WithReadClock supplies a deterministic clock for tests.
func WithReadClock(now func() time.Time) ReadAPIOption {
	return func(api *ReadAPI) {
		if now != nil {
			api.now = now
		}
	}
}

// WithOfflineAfter enables the documented last_seen-based offline derivation.
// A non-positive duration leaves online state unavailable rather than using an
// unsupported production default.
func WithOfflineAfter(after time.Duration) ReadAPIOption {
	return func(api *ReadAPI) { api.offlineAfter = after }
}

// NewReadAPI creates the read-side dependency that the router composes with
// the existing ingest endpoint.
func NewReadAPI(db *sql.DB, options ...ReadAPIOption) *ReadAPI {
	api := &ReadAPI{db: db, now: time.Now}
	for _, option := range options {
		option(api)
	}
	return api
}

// Overview returns the pilot's four KPI values and the actionable locations.
func (api *ReadAPI) Overview(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}

	locations, err := api.locations(r.Context(), "")
	if err != nil {
		api.writeReadError(w, err)
		return
	}

	response := overviewResponse{PriorityLocations: make([]locationListItem, 0), RecentActivity: make([]eventDTO, 0)}
	for _, location := range locations {
		response.TotalLocations++
		response.TotalDevices += len(location.Devices)
		for _, item := range location.Devices {
			if item.Status == nil {
				continue
			}
			switch device.Status(*item.Status) {
			case device.StatusSafetyAlert:
				response.SafetyAlerts++
			case device.StatusAngkat:
				response.PickupRequired++
			}
		}
		if location.Status == nil {
			continue
		}
		switch *location.Status {
		case string(device.StatusSafetyAlert):
			response.PriorityLocations = append(response.PriorityLocations, location.listItem())
		case string(device.StatusAngkat):
			response.PriorityLocations = append(response.PriorityLocations, location.listItem())
		case string(device.StatusWaspada):
			response.PriorityLocations = append(response.PriorityLocations, location.listItem())
		}
	}
	response.RecentActivity, err = api.recentEvents(r.Context())
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	sortLocationsByStatus(response.PriorityLocations)
	writeJSON(w, http.StatusOK, response)
}

// Locations lists locations and supports a simple case-insensitive search and
// exact canonical status filter.
func (api *ReadAPI) Locations(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	statusFilter := r.URL.Query().Get("status")
	if statusFilter != "" && !isPublicStatus(statusFilter) {
		writeError(w, http.StatusBadRequest, "invalid status filter")
		return
	}

	locations, err := api.locations(r.Context(), r.URL.Query().Get("q"))
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	items := make([]locationListItem, 0, len(locations))
	for _, location := range locations {
		if statusFilter != "" && (location.Status == nil || *location.Status != statusFilter) {
			continue
		}
		items = append(items, location.listItem())
	}
	writeJSON(w, http.StatusOK, map[string]any{"locations": items})
}

// Location returns a location and its public device summaries.
func (api *ReadAPI) Location(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	id := strings.TrimSpace(r.PathValue("id"))
	if id == "" {
		writeError(w, http.StatusBadRequest, "location id is required")
		return
	}
	location, err := api.location(r.Context(), id)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, location)
}

// Devices returns one flattened, read-only device summary per device. It is
// intended for monitoring workspaces that need location context without
// issuing one request per location from the browser.
func (api *ReadAPI) Devices(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	locations, err := api.locations(r.Context(), "")
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	items := make([]deviceSummaryDTO, 0)
	for _, location := range locations {
		for _, item := range location.Devices {
			items = append(items, deviceSummaryDTO{
				deviceDTO:       item,
				LocationName:    location.Name,
				LocationAddress: location.Address,
				LocationStatus:  location.Status,
			})
		}
	}
	writeJSON(w, http.StatusOK, map[string]any{"devices": items})
}

// Device returns one device's current operational summary.
func (api *ReadAPI) Device(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	id := strings.TrimSpace(r.PathValue("id"))
	if id == "" {
		writeError(w, http.StatusBadRequest, "device id is required")
		return
	}
	item, err := api.device(r.Context(), id)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	writeJSON(w, http.StatusOK, item.public())
}

// Research returns raw and normalized readings together with their auditable
// decision fields. gas_raw stays explicitly raw and is never labelled ppm.
func (api *ReadAPI) Research(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodGet {
		writeError(w, http.StatusMethodNotAllowed, "method not allowed")
		return
	}
	id := strings.TrimSpace(r.PathValue("id"))
	if id == "" {
		writeError(w, http.StatusBadRequest, "device id is required")
		return
	}
	item, err := api.device(r.Context(), id)
	if err != nil {
		api.writeReadError(w, err)
		return
	}
	research := make([]researchSlotDTO, 0, len(item.Slots))
	for _, slot := range item.Slots {
		research = append(research, slot.research())
	}
	writeJSON(w, http.StatusOK, map[string]any{"device_id": item.ID, "decision_summary": decisionSummary(item.Slots), "slots": research})
}

type overviewResponse struct {
	TotalLocations    int                `json:"total_locations"`
	TotalDevices      int                `json:"total_devices"`
	PickupRequired    int                `json:"pickup_required"`
	SafetyAlerts      int                `json:"safety_alerts"`
	PriorityLocations []locationListItem `json:"priority_locations"`
	RecentActivity    []eventDTO         `json:"recent_activity"`
}

type locationListItem struct {
	ID      string  `json:"id"`
	Name    string  `json:"name"`
	Address *string `json:"address_text"`
	Status  *string `json:"status"`
}

type locationDTO struct {
	ID      string      `json:"id"`
	Name    string      `json:"name"`
	Address *string     `json:"address_text"`
	Status  *string     `json:"status"`
	Devices []deviceDTO `json:"devices"`
}

type deviceDTO struct {
	ID                 string                  `json:"id"`
	LocationID         string                  `json:"location_id"`
	DeviceCode         string                  `json:"device_code"`
	BleDeviceID        *string                 `json:"ble_device_id,omitempty"`
	PlacementLabel     *string                 `json:"placement_label"`
	OnlineState        *string                 `json:"online_state"`
	LastSeen           *string                 `json:"last_seen"`
	Status             *string                 `json:"status"`
	Slots              []slotDTO               `json:"slots"`
	FirmwareMonitoring []firmwareMonitoringDTO `json:"firmware_monitoring"`
	FirmwareResults    []firmwareResultDTO     `json:"firmware_results"`
}

type deviceSummaryDTO struct {
	deviceDTO
	LocationName    string  `json:"location_name"`
	LocationAddress *string `json:"location_address_text"`
	LocationStatus  *string `json:"location_status"`
}

type slotDTO struct {
	Stream            string          `json:"stream"`
	Fill              *float64        `json:"fill"`
	Weight            *float64        `json:"weight"`
	WeightUtilization *float64        `json:"weight_utilization"`
	Status            *string         `json:"status"`
	DominantReason    *string         `json:"dominant_reason"`
	SafetyState       *string         `json:"safety_state"`
	RecordedAt        *string         `json:"recorded_at"`
	OGI               *float64        `json:"ogi"`
	AccumulationHours *float64        `json:"accumulation_hours"`
	Temperature       *float64        `json:"temperature"`
	SensorHealth      json.RawMessage `json:"-"`
	RawPayload        string          `json:"-"`
	FISScore          *float64        `json:"-"`
	FISStatus         *string         `json:"-"`
	FISVersion        *string         `json:"-"`
	TraceJSON         string          `json:"-"`
	SafetyJSON        string          `json:"-"`
}

type researchSlotDTO struct {
	Stream         string                 `json:"stream"`
	RecordedAt     *string                `json:"recorded_at"`
	Raw            rawReadingDTO          `json:"raw"`
	Normalized     normalizedDTO          `json:"normalized"`
	SensorHealth   json.RawMessage        `json:"sensor_health"`
	FISScore       *float64               `json:"fis_score"`
	FISStatus      *string                `json:"fis_status"`
	FISVersion     *string                `json:"fis_version"`
	DominantReason *string                `json:"dominant_reason"`
	SafetyState    *string                `json:"safety_state"`
	Sensors        []ingest.SensorReading `json:"sensors"`
	Derived        derivedParametersDTO   `json:"derived_parameters"`
	FISTrace       *ingest.DecisionTrace  `json:"fis_trace"`
	Safety         *ingest.SafetyEvidence `json:"safety"`
}

type derivedParametersDTO struct {
	OGI               *float64 `json:"ogi"`
	AccumulationHours *float64 `json:"accumulation_hours"`
}

type decisionSummaryDTO struct {
	PriorityScore  *float64 `json:"priority_score"`
	CriticalStream *string  `json:"critical_stream"`
	CriticalReason *string  `json:"critical_reason"`
	Mode           *string  `json:"mode"`
}

type rawReadingDTO struct {
	Fill        *float64 `json:"fill"`
	Weight      *float64 `json:"weight"`
	GasRaw      *float64 `json:"gas_raw"`
	Temperature *float64 `json:"temperature"`
}

type normalizedDTO struct {
	FillPct           *float64 `json:"fill_pct"`
	WeightUtilization *float64 `json:"weight_utilization"`
	OGI               *float64 `json:"ogi"`
	AccumulationHours *float64 `json:"accumulation_hours"`
	Temperature       *float64 `json:"temperature"`
}

type eventDTO struct {
	ID        int64   `json:"id"`
	DeviceID  string  `json:"device_id"`
	SlotID    *string `json:"slot_id"`
	EventType string  `json:"event_type"`
	Severity  string  `json:"severity"`
	StartedAt string  `json:"started_at"`
	EndedAt   *string `json:"ended_at"`
}

func (api *ReadAPI) locations(ctx context.Context, query string) ([]locationDTO, error) {
	if api == nil || api.db == nil {
		return nil, errors.New("read database is unavailable")
	}
	like := "%" + strings.TrimSpace(query) + "%"
	rows, err := api.db.QueryContext(ctx, `
		SELECT id FROM locations
		WHERE ? = '' OR lower(name) LIKE lower(?) OR lower(COALESCE(address_text, '')) LIKE lower(?)
		ORDER BY name, id`, strings.TrimSpace(query), like, like)
	if err != nil {
		return nil, err
	}
	ids := make([]string, 0)
	for rows.Next() {
		var id string
		if err := rows.Scan(&id); err != nil {
			rows.Close()
			return nil, err
		}
		ids = append(ids, id)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return nil, err
	}
	if err := rows.Close(); err != nil {
		return nil, err
	}

	locations := make([]locationDTO, 0, len(ids))
	for _, id := range ids {
		location, err := api.location(ctx, id)
		if err != nil {
			return nil, err
		}
		locations = append(locations, location)
	}
	return locations, nil
}

func (api *ReadAPI) location(ctx context.Context, id string) (locationDTO, error) {
	var result locationDTO
	if api == nil || api.db == nil {
		return result, errors.New("read database is unavailable")
	}
	var address sql.NullString
	err := api.db.QueryRowContext(ctx, `SELECT id, name, address_text FROM locations WHERE id = ?`, id).Scan(&result.ID, &result.Name, &address)
	if errors.Is(err, sql.ErrNoRows) {
		return result, errReadNotFound
	}
	if err != nil {
		return result, err
	}
	result.Address = nullableString(address)

	rows, err := api.db.QueryContext(ctx, `SELECT id FROM devices WHERE location_id = ? ORDER BY device_code, id`, id)
	if err != nil {
		return result, err
	}
	deviceIDs := make([]string, 0)
	result.Devices = make([]deviceDTO, 0)
	for rows.Next() {
		var deviceID string
		if err := rows.Scan(&deviceID); err != nil {
			rows.Close()
			return result, err
		}
		deviceIDs = append(deviceIDs, deviceID)
	}
	if err := rows.Err(); err != nil {
		rows.Close()
		return result, err
	}
	if err := rows.Close(); err != nil {
		return result, err
	}
	statuses := make([]device.Status, 0, len(deviceIDs))
	for _, deviceID := range deviceIDs {
		item, err := api.device(ctx, deviceID)
		if err != nil {
			return result, err
		}
		result.Devices = append(result.Devices, item.public())
		if item.Status != nil {
			statuses = append(statuses, device.Status(*item.Status))
		}
	}
	result.Status = aggregateStatus(statuses)
	return result, nil
}

func (api *ReadAPI) device(ctx context.Context, id string) (deviceDTO, error) {
	var result deviceDTO
	if api == nil || api.db == nil {
		return result, errors.New("read database is unavailable")
	}
	var placement, lastSeen, bleDeviceID sql.NullString
	err := api.db.QueryRowContext(ctx, `
		SELECT id, location_id, device_code, placement_label, last_seen, ble_device_id
		FROM devices WHERE id = ?`, id).Scan(&result.ID, &result.LocationID, &result.DeviceCode, &placement, &lastSeen, &bleDeviceID)
	if errors.Is(err, sql.ErrNoRows) {
		return result, errReadNotFound
	}
	if err != nil {
		return result, err
	}
	result.PlacementLabel = nullableString(placement)
	result.LastSeen = nullableString(lastSeen)
	result.BleDeviceID = nullableString(bleDeviceID)
	result.OnlineState = api.onlineState(lastSeen)

	slots, err := api.slots(ctx, result.ID)
	if err != nil {
		return result, err
	}
	result.Slots = slots
	result.FirmwareMonitoring, err = api.firmwareMonitoring(ctx, result.DeviceCode)
	if err != nil {
		return result, err
	}
	result.FirmwareResults, err = api.firmwareResults(ctx, result.DeviceCode)
	if err != nil {
		return result, err
	}
	statuses := make([]device.Status, 0, len(slots)+1)
	if result.OnlineState != nil && *result.OnlineState == "Offline" {
		statuses = append(statuses, device.StatusOffline)
	}
	for _, slot := range slots {
		if slot.Status != nil {
			statuses = append(statuses, device.Status(*slot.Status))
		}
	}
	result.Status = aggregateStatus(statuses)
	return result, nil
}

func (api *ReadAPI) slots(ctx context.Context, deviceID string) ([]slotDTO, error) {
	rows, err := api.db.QueryContext(ctx, `
		SELECT s.waste_stream,
			r.recorded_at, r.raw_payload, r.fill, r.weight, r.weight_utilization,
			r.ogi, r.accumulation, r.temperature, r.sensor_health,
			d.fis_score, d.fis_status, d.fis_version, d.dominant_reason, d.safety_state,
			d.trace_json, d.safety_json
		FROM slots s
		LEFT JOIN readings r ON r.id = (
			SELECT r2.id FROM readings r2 WHERE r2.slot_id = s.id
			ORDER BY r2.recorded_at DESC, r2.id DESC LIMIT 1
		)
		LEFT JOIN decisions d ON d.reading_id = r.id
		WHERE s.device_id = ?
		ORDER BY CASE s.waste_stream WHEN 'organic' THEN 1 WHEN 'inorganic' THEN 2 WHEN 'b3' THEN 3 ELSE 4 END`, deviceID)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	result := make([]slotDTO, 0, 3)
	for rows.Next() {
		var item slotDTO
		var recordedAt, rawPayload, health sql.NullString
		var fill, weight, utilization, ogi, accumulation, temperature, fisScore sql.NullFloat64
		var fisStatus, fisVersion, reason, safetyState, traceJSON, safetyJSON sql.NullString
		if err := rows.Scan(&item.Stream, &recordedAt, &rawPayload, &fill, &weight, &utilization, &ogi, &accumulation, &temperature, &health,
			&fisScore, &fisStatus, &fisVersion, &reason, &safetyState, &traceJSON, &safetyJSON); err != nil {
			return nil, err
		}
		item.RecordedAt = nullableString(recordedAt)
		item.RawPayload = rawPayload.String
		item.Fill = nullableFloat(fill)
		item.Weight = nullableFloat(weight)
		item.WeightUtilization = nullableFloat(utilization)
		item.OGI = nullableFloat(ogi)
		item.AccumulationHours = nullableFloat(accumulation)
		item.Temperature = nullableFloat(temperature)
		item.FISScore = nullableFloat(fisScore)
		item.FISStatus = nullableString(fisStatus)
		item.FISVersion = nullableString(fisVersion)
		item.DominantReason = nullableString(reason)
		item.SafetyState = nullableString(safetyState)
		item.TraceJSON = traceJSON.String
		item.SafetyJSON = safetyJSON.String
		if health.Valid && json.Valid([]byte(health.String)) {
			item.SensorHealth = json.RawMessage(health.String)
		} else {
			item.SensorHealth = json.RawMessage("null")
		}
		item.Status = finalStatus(item.FISStatus, item.SafetyState)
		result = append(result, item)
	}
	return result, rows.Err()
}

func (api *ReadAPI) recentEvents(ctx context.Context) ([]eventDTO, error) {
	rows, err := api.db.QueryContext(ctx, `
		SELECT id, device_id, slot_id, event_type, severity, started_at, ended_at
		FROM events ORDER BY started_at DESC, id DESC LIMIT 3`)
	if err != nil {
		return nil, err
	}
	defer rows.Close()
	result := make([]eventDTO, 0)
	for rows.Next() {
		var item eventDTO
		var slotID, endedAt sql.NullString
		if err := rows.Scan(&item.ID, &item.DeviceID, &slotID, &item.EventType, &item.Severity, &item.StartedAt, &endedAt); err != nil {
			return nil, err
		}
		item.SlotID = nullableString(slotID)
		item.EndedAt = nullableString(endedAt)
		result = append(result, item)
	}
	return result, rows.Err()
}

func (api *ReadAPI) onlineState(lastSeen sql.NullString) *string {
	if api.offlineAfter <= 0 {
		return nil
	}
	var heartbeat *time.Time
	if lastSeen.Valid {
		if parsed, err := time.Parse(time.RFC3339Nano, lastSeen.String); err == nil {
			heartbeat = &parsed
		}
	}
	offline, err := device.IsOffline(heartbeat, api.now().UTC(), device.OfflineConfig{OfflineAfter: api.offlineAfter})
	if err != nil {
		return nil
	}
	if offline {
		return stringPointer("Offline")
	}
	return stringPointer("Online")
}

func (api *ReadAPI) writeReadError(w http.ResponseWriter, err error) {
	if errors.Is(err, errReadNotFound) {
		writeError(w, http.StatusNotFound, "not found")
		return
	}
	writeError(w, http.StatusInternalServerError, "internal server error")
}

var errReadNotFound = errors.New("read resource not found")

func (item locationDTO) listItem() locationListItem {
	return locationListItem{ID: item.ID, Name: item.Name, Address: item.Address, Status: item.Status}
}

func (item deviceDTO) public() deviceDTO {
	for index := range item.Slots {
		item.Slots[index].RawPayload = ""
		item.Slots[index].SensorHealth = nil
		item.Slots[index].FISScore = nil
		item.Slots[index].FISStatus = nil
		item.Slots[index].FISVersion = nil
		item.Slots[index].OGI = nil
		item.Slots[index].AccumulationHours = nil
		item.Slots[index].Temperature = nil
	}
	return item
}

func (item slotDTO) research() researchSlotDTO {
	payloadSlot := rawSlot(item.RawPayload, item.Stream)
	var trace *ingest.DecisionTrace
	if item.TraceJSON != "" {
		var decoded ingest.DecisionTrace
		if json.Unmarshal([]byte(item.TraceJSON), &decoded) == nil {
			trace = &decoded
		}
	}
	var safetyEvidence *ingest.SafetyEvidence
	if item.SafetyJSON != "" {
		var decoded ingest.SafetyEvidence
		if json.Unmarshal([]byte(item.SafetyJSON), &decoded) == nil {
			safetyEvidence = &decoded
		}
	}
	return researchSlotDTO{
		Stream:     item.Stream,
		RecordedAt: item.RecordedAt,
		Raw: rawReadingDTO{
			Fill: item.Fill, Weight: item.Weight, GasRaw: rawGas(item.RawPayload, item.Stream), Temperature: item.Temperature,
		},
		Normalized: normalizedDTO{
			FillPct: item.Fill, WeightUtilization: item.WeightUtilization, OGI: item.OGI, AccumulationHours: item.AccumulationHours, Temperature: item.Temperature,
		},
		SensorHealth: item.SensorHealth,
		FISScore:     item.FISScore, FISStatus: item.FISStatus, FISVersion: item.FISVersion,
		DominantReason: item.DominantReason, SafetyState: item.SafetyState,
		Sensors:  payloadSlot.Sensors,
		Derived:  derivedParametersDTO{OGI: item.OGI, AccumulationHours: item.AccumulationHours},
		FISTrace: trace,
		Safety:   safetyEvidence,
	}
}

func rawSlot(payload, stream string) ingest.Slot {
	var request ingest.Request
	if json.Unmarshal([]byte(payload), &request) != nil {
		return ingest.Slot{}
	}
	for _, slot := range request.Slots {
		if slot.Stream == stream {
			return slot
		}
	}
	return ingest.Slot{}
}

func rawGas(payload, stream string) *float64 {
	return rawSlot(payload, stream).GasRaw
}

func decisionSummary(slots []slotDTO) decisionSummaryDTO {
	var result decisionSummaryDTO
	var critical *slotDTO
	for index := range slots {
		slot := &slots[index]
		if slot.FISScore != nil && (result.PriorityScore == nil || *slot.FISScore > *result.PriorityScore) {
			result.PriorityScore = slot.FISScore
		}
		if traceHasFourInputs(slot.TraceJSON) {
			result.Mode = stringPointer("Fuzzy multi-parameter")
		}
		if critical == nil || compareSlotPriority(*slot, *critical) > 0 {
			critical = slot
		}
	}
	if critical != nil && critical.Status != nil {
		result.CriticalStream = stringPointer(critical.Stream)
		result.CriticalReason = critical.DominantReason
		if critical.SafetyState != nil && *critical.SafetyState == safety.StateSafetyAlert && critical.SafetyJSON != "" {
			var evidence ingest.SafetyEvidence
			if json.Unmarshal([]byte(critical.SafetyJSON), &evidence) == nil && strings.TrimSpace(evidence.Reason) != "" {
				result.CriticalReason = stringPointer(evidence.Reason)
			}
		}
	}
	return result
}

func compareSlotPriority(left, right slotDTO) int {
	leftStatus, rightStatus := device.Status(""), device.Status("")
	if left.Status != nil {
		leftStatus = device.Status(*left.Status)
	}
	if right.Status != nil {
		rightStatus = device.Status(*right.Status)
	}
	if compared := device.Compare(leftStatus, rightStatus); compared != 0 {
		return compared
	}
	leftScore, rightScore := -1.0, -1.0
	if left.FISScore != nil {
		leftScore = *left.FISScore
	}
	if right.FISScore != nil {
		rightScore = *right.FISScore
	}
	switch {
	case leftScore > rightScore:
		return 1
	case leftScore < rightScore:
		return -1
	default:
		return 0
	}
}

func traceHasFourInputs(raw string) bool {
	if raw == "" {
		return false
	}
	var trace ingest.DecisionTrace
	if json.Unmarshal([]byte(raw), &trace) != nil {
		return false
	}
	for _, name := range []string{"fill", "weight_utilization", "ogi", "accumulation_hours"} {
		if _, ok := trace.Inputs[name]; !ok {
			return false
		}
	}
	return len(trace.Inputs) == 4
}

func finalStatus(fisStatus, safetyState *string) *string {
	if safetyState != nil && *safetyState == safety.StateSafetyAlert {
		return stringPointer(string(device.StatusSafetyAlert))
	}
	if fisStatus != nil && isPublicStatus(*fisStatus) {
		return stringPointer(*fisStatus)
	}
	return nil
}

func aggregateStatus(statuses []device.Status) *string {
	if len(statuses) == 0 {
		return nil
	}
	return stringPointer(string(device.Aggregate(statuses...)))
}

func isPublicStatus(value string) bool {
	switch device.Status(value) {
	case device.StatusNormal, device.StatusWaspada, device.StatusAngkat, device.StatusSafetyAlert, device.StatusOffline, device.StatusDataInvalid:
		return true
	default:
		return false
	}
}

func nullableString(value sql.NullString) *string {
	if !value.Valid {
		return nil
	}
	return stringPointer(value.String)
}

func nullableFloat(value sql.NullFloat64) *float64 {
	if !value.Valid {
		return nil
	}
	result := value.Float64
	return &result
}

func stringPointer(value string) *string { return &value }

func sortLocationsByStatus(items []locationListItem) {
	for i := 0; i < len(items); i++ {
		for j := i + 1; j < len(items); j++ {
			left, right := device.Status(items[i].StatusValue()), device.Status(items[j].StatusValue())
			if device.Compare(right, left) > 0 || (device.Compare(right, left) == 0 && items[j].Name < items[i].Name) {
				items[i], items[j] = items[j], items[i]
			}
		}
	}
}

func (item locationListItem) StatusValue() string {
	if item.Status == nil {
		return ""
	}
	return *item.Status
}
