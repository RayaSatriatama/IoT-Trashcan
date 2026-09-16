package main

import (
	"log"
	"net/http"
	"os"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
)

func main() {
	dbPath := envOrDefault("SIGAP_DB_PATH", "sigap.db")
	addr := envOrDefault("SIGAP_HTTP_ADDR", ":8080")

	db, err := database.Open(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer db.Close()

	server := &http.Server{Addr: addr, Handler: httpapi.NewRouter(ingest.NewService(db))}
	log.Printf("SIGAP listening on %s", addr)
	if err := server.ListenAndServe(); err != nil && err != http.ErrServerClosed {
		log.Fatalf("serve HTTP: %v", err)
	}
}

func envOrDefault(name, fallback string) string {
	if value := os.Getenv(name); value != "" {
		return value
	}
	return fallback
}
