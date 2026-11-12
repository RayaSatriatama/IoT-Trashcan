package main

import (
	"net/http"
	"net/http/httptest"
	"testing"
)

func TestCORSBoundary(t *testing.T) {
	cases := []struct {
		name, config, origin, want string
		creds                      bool
		noConfig                   bool
	}{
		{"default env", "", "http://localhost:8081", "http://localhost:8081", true, true},
		{"scheme difference", "http://localhost:8081", "https://localhost:8081", "", false, false},
		{"port difference", "https://a.example", "https://a.example:444", "", false, false},
		{"prefix", "https://console.example.com", "https://console.example.co", "", false, false},
		{"suffix", "https://console.example.com", "https://console.example.com.attacker.invalid", "", false, false},
		{"trim multi", " https://one.example , https://two.example ", "https://two.example", "https://two.example", true, false},
		{"public wildcard", "*", "https://other.example", "*", false, false},
		{"invalid configured", "https://u@a.example/path,https://a.example?x,https://a.example#x", "https://a.example", "", false, false},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			if tc.noConfig {
				t.Setenv("ALLOWED_ORIGIN", "")
			} else {
				t.Setenv("ALLOWED_ORIGIN", tc.config)
			}
			h := corsMiddleware(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(202) }))
			r := httptest.NewRequest("GET", "/", nil)
			r.Header.Set("Origin", tc.origin)
			w := httptest.NewRecorder()
			h.ServeHTTP(w, r)
			if got := w.Header().Get("Access-Control-Allow-Origin"); got != tc.want {
				t.Errorf("ACAO=%q want %q", got, tc.want)
			}
			if (w.Header().Get("Access-Control-Allow-Credentials") == "true") != tc.creds {
				t.Errorf("credentials incorrect")
			}
			if w.Header().Get("Vary") != "Origin" {
				t.Errorf("Vary=%q", w.Header().Get("Vary"))
			}
			if w.Code != 202 {
				t.Errorf("status=%d", w.Code)
			}
		})
	}
	for _, origin := range []string{"https://u@a.example", "https://a.example/path", "https://a.example?x", "https://a.example#x", "https://a.example?", "https://a.example#", "null", "https://a.example,https://b.example"} {
		t.Run("malformed "+origin, func(t *testing.T) {
			t.Setenv("ALLOWED_ORIGIN", "https://a.example")
			r := httptest.NewRequest("GET", "/", nil)
			r.Header.Set("Origin", origin)
			w := httptest.NewRecorder()
			corsMiddleware(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(202) })).ServeHTTP(w, r)
			if w.Header().Get("Access-Control-Allow-Origin") != "" {
				t.Errorf("unexpected ACAO")
			}
		})
	}
	t.Run("duplicate origins", func(t *testing.T) {
		t.Setenv("ALLOWED_ORIGIN", "https://a.example")
		r := httptest.NewRequest("GET", "/", nil)
		r.Header.Add("Origin", "https://a.example")
		r.Header.Add("Origin", "https://a.example")
		w := httptest.NewRecorder()
		corsMiddleware(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(202) })).ServeHTTP(w, r)
		if w.Header().Get("Access-Control-Allow-Origin") != "" {
			t.Error("duplicate accepted")
		}
	})
	t.Run("vary preserve absent origin", func(t *testing.T) {
		t.Setenv("ALLOWED_ORIGIN", "https://a.example")
		r := httptest.NewRequest("GET", "/", nil)
		w := httptest.NewRecorder()
		w.Header().Set("Vary", "Accept-Encoding")
		corsMiddleware(http.HandlerFunc(func(w http.ResponseWriter, _ *http.Request) { w.WriteHeader(202) })).ServeHTTP(w, r)
		if got := w.Header().Values("Vary"); len(got) != 2 || got[0] != "Accept-Encoding" || got[1] != "Origin" {
			t.Errorf("Vary=%v", got)
		}
	})
	for _, allowed := range []bool{true, false} {
		t.Run("preflight", func(t *testing.T) {
			t.Setenv("ALLOWED_ORIGIN", "https://a.example")
			r := httptest.NewRequest("OPTIONS", "/", nil)
			if allowed {
				r.Header.Set("Origin", "https://a.example")
			} else {
				r.Header.Set("Origin", "https://evil.example")
			}
			hit := false
			w := httptest.NewRecorder()
			corsMiddleware(http.HandlerFunc(func(http.ResponseWriter, *http.Request) { hit = true })).ServeHTTP(w, r)
			if w.Code != 204 || hit {
				t.Errorf("code=%d hit=%v", w.Code, hit)
			}
			if (w.Header().Get("Access-Control-Allow-Origin") == "https://a.example") != allowed {
				t.Errorf("ACAO=%q", w.Header().Get("Access-Control-Allow-Origin"))
			}
		})
	}
}
