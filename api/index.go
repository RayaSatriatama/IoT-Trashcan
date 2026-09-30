package handler

import (
	"net/http"
	"strings"
	"sync"

	"github.com/RayaSatriatama/IoT-Trashcan/vercelsnapshot"
)

var (
	initialize sync.Once
	handler    http.Handler
	initErr    error
)

// Handler is the Vercel Go Function entrypoint for every rewritten /api/* request.
func Handler(w http.ResponseWriter, r *http.Request) {
	initialize.Do(func() {
		handler, initErr = newSnapshotHandler()
	})
	if initErr != nil {
		http.Error(w, "simulation snapshot unavailable", http.StatusServiceUnavailable)
		return
	}
	path := strings.TrimPrefix(r.URL.Query().Get("path"), "/")
	if path != "" {
		r = r.Clone(r.Context())
		r.URL.Path = "/api/" + path
		r.URL.RawQuery = r.URL.Query().Encode()
	}
	handler.ServeHTTP(w, r)
}

func newSnapshotHandler() (http.Handler, error) {
	return vercelsnapshot.NewHandlerFromEnvironment()
}
