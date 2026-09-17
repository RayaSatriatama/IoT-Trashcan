package webapp

import (
	"io"
	"io/fs"
	"net/http"
	"net/http/httptest"
	"testing"
	"time"
)

func TestHandlerServesSPARoutesAndStaticAssets(t *testing.T) {
	staticFS := fstestMapFS{
		"index.html":      "<!doctype html><html><body>SIGAP</body></html>",
		"assets/app.js":   "console.log('sigap')",
		"assets/logo.svg": "<svg></svg>",
	}
	api := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.URL.Path == "/api/overview" {
			w.Header().Set("Content-Type", "application/json")
			w.WriteHeader(http.StatusOK)
			_, _ = w.Write([]byte(`{"ok":true}`))
			return
		}
		http.NotFound(w, r)
	})
	handler := NewHandler(staticFS, api)

	for _, route := range []string{"/", "/history", "/locations/location-a", "/devices/device-a"} {
		recorder := httptest.NewRecorder()
		handler.ServeHTTP(recorder, httptest.NewRequest(http.MethodGet, route, nil))
		if recorder.Code != http.StatusOK || recorder.Body.String() == "" {
			t.Fatalf("route %s: status=%d body=%q", route, recorder.Code, recorder.Body.String())
		}
	}

	asset := httptest.NewRecorder()
	handler.ServeHTTP(asset, httptest.NewRequest(http.MethodGet, "/assets/app.js", nil))
	if asset.Code != http.StatusOK || asset.Body.String() != "console.log('sigap')" {
		t.Fatalf("asset: status=%d body=%q", asset.Code, asset.Body.String())
	}
}

func TestHandlerKeepsAPIPrecedenceAndReturnsMissingAsset404(t *testing.T) {
	staticFS := fstestMapFS{"index.html": "<!doctype html>SPA"}
	api := http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Error(w, "api not found", http.StatusNotFound)
	})
	handler := NewHandler(staticFS, api)

	apiRecorder := httptest.NewRecorder()
	handler.ServeHTTP(apiRecorder, httptest.NewRequest(http.MethodGet, "/api/not-real", nil))
	if apiRecorder.Code != http.StatusNotFound || apiRecorder.Body.String() == "<!doctype html>SPA" {
		t.Fatalf("API fallback swallowed: status=%d body=%q", apiRecorder.Code, apiRecorder.Body.String())
	}

	missing := httptest.NewRecorder()
	handler.ServeHTTP(missing, httptest.NewRequest(http.MethodGet, "/assets/missing.js", nil))
	if missing.Code != http.StatusNotFound {
		t.Fatalf("missing asset status=%d, want %d", missing.Code, http.StatusNotFound)
	}
}

func TestHandlerRejectsTraversalAndUnsupportedMethods(t *testing.T) {
	staticFS := fstestMapFS{"index.html": "<!doctype html>SPA"}
	handler := NewHandler(staticFS, http.NotFoundHandler())

	traversal := httptest.NewRecorder()
	handler.ServeHTTP(traversal, httptest.NewRequest(http.MethodGet, "/../index.html", nil))
	if traversal.Code != http.StatusNotFound {
		t.Fatalf("traversal status=%d, want %d", traversal.Code, http.StatusNotFound)
	}

	post := httptest.NewRecorder()
	handler.ServeHTTP(post, httptest.NewRequest(http.MethodPost, "/history", nil))
	if post.Code != http.StatusNotFound {
		t.Fatalf("POST SPA route status=%d, want %d", post.Code, http.StatusNotFound)
	}
}

// fstestMapFS is a tiny local fs.FS fixture that keeps this package test
// independent from generated Expo output.
type fstestMapFS map[string]string

func (m fstestMapFS) Open(name string) (fs.File, error) {
	data, ok := m[name]
	if !ok {
		return nil, fs.ErrNotExist
	}
	return &mapFile{name: name, data: []byte(data)}, nil
}

type mapFile struct {
	name string
	data []byte
	pos  int64
}

func (f *mapFile) Close() error { return nil }

func (f *mapFile) Read(p []byte) (int, error) {
	if f.pos >= int64(len(f.data)) {
		return 0, io.EOF
	}
	n := copy(p, f.data[f.pos:])
	f.pos += int64(n)
	return n, nil
}

func (f *mapFile) Stat() (fs.FileInfo, error) {
	return mapFileInfo{name: f.name, size: int64(len(f.data))}, nil
}

func (f *mapFile) Seek(offset int64, whence int) (int64, error) {
	var next int64
	switch whence {
	case 0:
		next = offset
	case 1:
		next = f.pos + offset
	case 2:
		next = int64(len(f.data)) + offset
	default:
		return f.pos, fs.ErrInvalid
	}
	if next < 0 {
		return f.pos, fs.ErrInvalid
	}
	f.pos = next
	return f.pos, nil
}

type mapFileInfo struct {
	name string
	size int64
}

func (i mapFileInfo) Name() string       { return i.name }
func (i mapFileInfo) Size() int64        { return i.size }
func (i mapFileInfo) Mode() fs.FileMode  { return 0 }
func (i mapFileInfo) ModTime() time.Time { return time.Time{} }
func (i mapFileInfo) IsDir() bool        { return false }
func (i mapFileInfo) Sys() interface{}   { return nil }
