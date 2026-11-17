package httpapi

import (
	"errors"
	"net/http"
	"strconv"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
)

func newSensorHistoryHandler(api *ReadAPI, query *telemetryarchive.ArchiveQuery) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet {
			writeError(w, http.StatusMethodNotAllowed, "method not allowed")
			return
		}
		id, sensor, boot := r.PathValue("id"), r.URL.Query().Get("sensor_id"), r.URL.Query().Get("boot_id")
		if id == "" || sensor == "" {
			writeError(w, http.StatusBadRequest, "invalid history query")
			return
		}
		limit := 200
		if v := r.URL.Query().Get("limit"); v != "" {
			n, e := strconv.Atoi(v)
			if e != nil || n < 1 {
				writeError(w, http.StatusBadRequest, "invalid history query")
				return
			}
			limit = n
		}
		device, err := api.device(r.Context(), id)
		if err != nil {
			api.writeReadError(w, err)
			return
		}
		if boot == "" {
			for _, current := range device.FirmwareMonitoring {
				if current.SensorID == sensor {
					boot = current.BootID
					break
				}
			}
		}
		page, err := query.ListSensorHistory(r.Context(), device.DeviceCode, sensor, boot, r.URL.Query().Get("cursor"), limit)
		if errors.Is(err, telemetryarchive.ErrInvalidQuery) {
			writeError(w, http.StatusBadRequest, "invalid history query")
			return
		}
		if err != nil {
			api.writeReadError(w, err)
			return
		}
		page.DeviceID = id
		writeJSON(w, http.StatusOK, page)
	})
}
