package main

import (
	"fmt"
	"os"
)

// serverConfig contains process-local server settings. TLS is opt-in so the
// existing local development entrypoint remains compatible, while staging can
// fail closed when HTTPS is required.
type serverConfig struct {
	dbPath      string
	addr        string
	tlsCertFile string
	tlsKeyFile  string
}

func loadServerConfig(getenv func(string) string) (serverConfig, error) {
	config := serverConfig{
		dbPath: getenvOrDefault(getenv, "SIGAP_DB_PATH", "sigap.db"),
		addr:   getenvOrDefault(getenv, "SIGAP_HTTP_ADDR", ":8080"),
	}
	config.tlsCertFile = getenv("SIGAP_TLS_CERT_FILE")
	config.tlsKeyFile = getenv("SIGAP_TLS_KEY_FILE")
	tlsRequired, err := parseBoolEnv(getenv, "SIGAP_TLS_REQUIRED")
	if err != nil {
		return serverConfig{}, err
	}
	if (config.tlsCertFile == "") != (config.tlsKeyFile == "") {
		return serverConfig{}, fmt.Errorf("SIGAP_TLS_CERT_FILE and SIGAP_TLS_KEY_FILE must be set together")
	}
	if tlsRequired && config.tlsCertFile == "" {
		return serverConfig{}, fmt.Errorf("SIGAP_TLS_REQUIRED requires SIGAP_TLS_CERT_FILE and SIGAP_TLS_KEY_FILE")
	}
	return config, nil
}

func (config serverConfig) tlsEnabled() bool {
	return config.tlsCertFile != "" && config.tlsKeyFile != ""
}

func (config serverConfig) validateTLSFiles() error {
	if !config.tlsEnabled() {
		return nil
	}
	for name, path := range map[string]string{
		"certificate": config.tlsCertFile,
		"key":         config.tlsKeyFile,
	} {
		info, err := os.Stat(path)
		if err != nil {
			return fmt.Errorf("TLS %s file: %w", name, err)
		}
		if !info.Mode().IsRegular() {
			return fmt.Errorf("TLS %s file is not regular: %s", name, path)
		}
	}
	return nil
}

func getenvOrDefault(getenv func(string) string, name, fallback string) string {
	if value := getenv(name); value != "" {
		return value
	}
	return fallback
}

func parseBoolEnv(getenv func(string) string, name string) (bool, error) {
	value := getenv(name)
	if value == "" {
		return false, nil
	}
	if value == "true" {
		return true, nil
	}
	if value == "false" {
		return false, nil
	}
	return false, fmt.Errorf("%s must be true or false: %q", name, value)
}
