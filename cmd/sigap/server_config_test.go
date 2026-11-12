package main

import (
	"os"
	"path/filepath"
	"testing"
)

func envMap(values map[string]string) func(string) string {
	return func(name string) string { return values[name] }
}

func TestLoadServerConfigDefaultsToExistingHTTPEntrypoint(t *testing.T) {
	config, err := loadServerConfig(envMap(nil))
	if err != nil {
		t.Fatalf("loadServerConfig() error = %v", err)
	}
	if config.addr != ":8080" || config.dbPath != "sigap.db" || config.tlsEnabled() {
		t.Fatalf("unexpected defaults: %+v", config)
	}
}

func TestLoadServerConfigRequiresCompleteTLSPair(t *testing.T) {
	_, err := loadServerConfig(envMap(map[string]string{
		"SIGAP_TLS_CERT_FILE": "cert.pem",
	}))
	if err == nil {
		t.Fatal("expected incomplete TLS configuration to be rejected")
	}
}

func TestLoadServerConfigTLSRequiredFailsClosed(t *testing.T) {
	_, err := loadServerConfig(envMap(map[string]string{
		"SIGAP_TLS_REQUIRED": "true",
	}))
	if err == nil {
		t.Fatal("expected required TLS without certificate pair to be rejected")
	}
}

func TestTLSConfigValidatesRegularFiles(t *testing.T) {
	dir := t.TempDir()
	cert := filepath.Join(dir, "cert.pem")
	key := filepath.Join(dir, "key.pem")
	if err := os.WriteFile(cert, []byte("test certificate"), 0o600); err != nil {
		t.Fatal(err)
	}
	if err := os.WriteFile(key, []byte("test key"), 0o600); err != nil {
		t.Fatal(err)
	}
	config, err := loadServerConfig(envMap(map[string]string{
		"SIGAP_TLS_CERT_FILE": cert,
		"SIGAP_TLS_KEY_FILE":  key,
		"SIGAP_TLS_REQUIRED":  "true",
		"SIGAP_HTTP_ADDR":     "127.0.0.1:18443",
		"SIGAP_DB_PATH":       filepath.Join(dir, "staging.db"),
	}))
	if err != nil {
		t.Fatalf("loadServerConfig() error = %v", err)
	}
	if !config.tlsEnabled() || config.addr != "127.0.0.1:18443" {
		t.Fatalf("unexpected TLS config: %+v", config)
	}
	if err := config.validateTLSFiles(); err != nil {
		t.Fatalf("validateTLSFiles() error = %v", err)
	}
}

func TestTLSConfigRejectsMissingFileBeforeServerStart(t *testing.T) {
	config, err := loadServerConfig(envMap(map[string]string{
		"SIGAP_TLS_CERT_FILE": "/missing/cert.pem",
		"SIGAP_TLS_KEY_FILE":  "/missing/key.pem",
	}))
	if err != nil {
		t.Fatalf("loadServerConfig() error = %v", err)
	}
	if err := config.validateTLSFiles(); err == nil {
		t.Fatal("expected missing TLS file to be rejected")
	}
}
