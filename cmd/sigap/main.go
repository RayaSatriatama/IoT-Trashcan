package main

import (
	"log"
	"net/http"
	"os"
	"strconv"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/webapp"
)

func main() {
	dbPath := envOrDefault("SIGAP_DB_PATH", "sigap.db")
	addr := envOrDefault("SIGAP_HTTP_ADDR", ":8080")

	db, err := database.Open(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer db.Close()

	readOptions := readAPIOptions()
	service := ingest.NewService(db)
	log.Print("production FIS and safety configuration is unset; valid ingests persist raw readings without fabricated decisions")
	apiHandler := httpapi.NewRouter(service, httpapi.WithReadDatabase(db, readOptions...))
	handler := webapp.Handler(apiHandler)
	server := &http.Server{Addr: addr, Handler: handler}
	log.Printf("SIGAP listening on %s", addr)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("serve HTTP: %v", err)
	}
}

func readAPIOptions() []httpapi.ReadAPIOption {
	value := os.Getenv("OFFLINE_AFTER_SECONDS")
	if value == "" {
		log.Print("OFFLINE_AFTER_SECONDS is unset; offline derivation is disabled")
		return nil
	}
	seconds, err := strconv.ParseInt(value, 10, 64)
	if err != nil || seconds <= 0 {
		log.Fatalf("OFFLINE_AFTER_SECONDS must be a positive integer: %q", value)
	}
	return []httpapi.ReadAPIOption{httpapi.WithOfflineAfter(time.Duration(seconds) * time.Second)}
}

func envOrDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
