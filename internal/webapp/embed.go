// Package webapp serves the exported React Native Web application from the
// same process as the SIGAP API.
package webapp

import (
	"bytes"
	"embed"
	"io"
	"io/fs"
	"net/http"
	"path"
	"strings"
)

// EmbeddedFiles contains the frontend export staged under dist by the release
// build. The tracked placeholder keeps clean-checkout Go builds compilable;
// release builds replace generated files in this directory temporarily.
//
// The `all:` prefix is required because Expo's production bundle lives under
// `_expo`, and Go's default embed patterns omit underscore-prefixed paths.
//
//go:embed all:dist
var EmbeddedFiles embed.FS

// Handler wraps the API handler with the embedded frontend. API paths always
// take precedence over the SPA fallback.
func Handler(api http.Handler) http.Handler {
	staticFS, err := fs.Sub(EmbeddedFiles, "dist")
	if err != nil {
		panic("webapp: embedded dist is unavailable")
	}
	return NewHandler(staticFS, api)
}

// NewHandler composes a static filesystem with the API handler. It is
// exported so tests and integration harnesses can provide a deterministic
// fixture filesystem without rebuilding the frontend bundle.
func NewHandler(staticFS fs.FS, api http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if isAPIPath(r.URL.Path) {
			if api == nil {
				http.NotFound(w, r)
				return
			}
			api.ServeHTTP(w, r)
			return
		}

		if r.Method != http.MethodGet && r.Method != http.MethodHead {
			http.NotFound(w, r)
			return
		}

		requestPath, valid := cleanRequestPath(r.URL.Path)
		if !valid {
			http.NotFound(w, r)
			return
		}
		if requestPath == "" {
			requestPath = "index.html"
		}

		if serveFile(w, r, staticFS, requestPath) {
			return
		}

		// Missing files with an extension, and all missing assets, are real 404s.
		// Extensionless paths are handed to the SPA so Expo Router can resolve
		// deep links such as /history and /devices/device-a on refresh.
		if path.Ext(requestPath) != "" || strings.HasPrefix(requestPath, "assets/") {
			http.NotFound(w, r)
			return
		}
		if !serveFile(w, r, staticFS, "index.html") {
			http.Error(w, "frontend bundle unavailable", http.StatusServiceUnavailable)
		}
	})
}

func isAPIPath(requestPath string) bool {
	return requestPath == "/api" || strings.HasPrefix(requestPath, "/api/")
}

func cleanRequestPath(requestPath string) (string, bool) {
	for _, segment := range strings.Split(strings.Trim(requestPath, "/"), "/") {
		if segment == ".." {
			return "", false
		}
	}
	cleaned := strings.TrimPrefix(path.Clean("/"+requestPath), "/")
	if cleaned == "." || strings.HasPrefix(cleaned, "../") {
		return "", false
	}
	if !fs.ValidPath(cleaned) && cleaned != "" {
		return "", false
	}
	return cleaned, true
}

func serveFile(w http.ResponseWriter, r *http.Request, staticFS fs.FS, name string) bool {
	file, err := staticFS.Open(name)
	if err != nil {
		return false
	}
	defer file.Close()
	info, err := file.Stat()
	if err != nil || info.IsDir() {
		return false
	}
	reader, ok := file.(io.ReadSeeker)
	if !ok {
		data, readErr := io.ReadAll(file)
		if readErr != nil {
			return false
		}
		reader = bytes.NewReader(data)
	}
	http.ServeContent(w, r, name, info.ModTime(), reader)
	return true
}
