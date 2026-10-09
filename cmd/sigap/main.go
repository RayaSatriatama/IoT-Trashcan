package main

import (
	"context"
	"errors"
	"fmt"
	"log"
	"net/http"
	"os"
	"os/signal"
	"strconv"
	"syscall"
	"time"

	"github.com/RayaSatriatama/IoT-Trashcan/internal/database"
	httpapi "github.com/RayaSatriatama/IoT-Trashcan/internal/http"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/ingest"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/simulator"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/telemetryarchive"
	"github.com/RayaSatriatama/IoT-Trashcan/internal/webapp"
)

func main() {
	serverConfig, err := loadServerConfig(os.Getenv)
	if err != nil {
		log.Fatalf("server configuration: %v", err)
	}
	if err := serverConfig.validateTLSFiles(); err != nil {
		log.Fatalf("server TLS configuration: %v", err)
	}
	dbPath := serverConfig.dbPath
	addr := serverConfig.addr

	db, err := database.Open(dbPath)
	if err != nil {
		log.Fatalf("open database: %v", err)
	}
	defer db.Close()

	simulationConfig, err := simulator.ParseEnvironment()
	if err != nil {
		log.Fatalf("simulation configuration: %v", err)
	}
	simulationIndicatorEnabled, err := envBoolOrDefault("SIGAP_SIMULATION_INDICATOR_ENABLED", true)
	if err != nil {
		log.Fatalf("simulation indicator configuration: %v", err)
	}
	readOptions := readAPIOptions()
	serviceOptions := make([]ingest.ServiceOption, 0, 2)
	var service *ingest.Service
	var simulation *simulator.Simulator
	if simulationConfig.Enabled {
		dataset, seedErr := simulator.Seed(context.Background(), db, simulationConfig.Seed)
		if seedErr != nil {
			log.Fatalf("seed simulation dataset: %v", seedErr)
		}
		serviceOptions = append(serviceOptions, ingest.WithDecisionProfiles(simulator.DemoEngines()), ingest.WithSafetyConfig(simulator.DemoSafetyConfig()))
		service = ingest.NewService(db, serviceOptions...)
		simulation = simulator.New(service, dataset, simulationConfig)
		log.Printf("================================")
		log.Printf("SIGAP SIMULATION MODE ENABLED")
		log.Printf("NO PHYSICAL DEVICE REQUIRED")
		log.Printf("================================")
		log.Printf("simulation interval=%s seed=%d devices=%d database=%s", simulationConfig.Interval, simulationConfig.Seed, len(dataset.Devices), dbPath)
	} else {
		service = ingest.NewService(db)
		log.Print("production FIS and safety configuration is unset; valid ingests persist raw readings without fabricated decisions")
	}
	archiveService := telemetryarchive.NewService(db)
	archiveQuery, err := telemetryarchive.NewArchiveQuery(db)
	if err != nil {
		log.Fatalf("configure telemetry archive query: %v", err)
	}
	apiHandler := httpapi.NewRouter(
		service,
		httpapi.WithReadDatabase(db, readOptions...),
		httpapi.WithSimulationEnabled(simulationConfig.Enabled),
		httpapi.WithSimulationIndicatorEnabled(simulationIndicatorEnabled),
		httpapi.WithTelemetryArchive(archiveService, archiveQuery),
	)
	handler := webapp.Handler(apiHandler)
	server := &http.Server{Addr: addr, Handler: handler}
	log.Printf("SIGAP listening on %s", addr)
	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()
	if simulation != nil {
		go func() {
			if runErr := simulation.Run(ctx); runErr != nil && !errors.Is(runErr, context.Canceled) {
				log.Printf("simulation stopped: %v", runErr)
			}
		}()
	}
	serverErr := make(chan error, 1)
	go func() {
		if serverConfig.tlsEnabled() {
			serverErr <- server.ListenAndServeTLS(serverConfig.tlsCertFile, serverConfig.tlsKeyFile)
			return
		}
		serverErr <- server.ListenAndServe()
	}()
	select {
	case serveErr := <-serverErr:
		if serveErr != nil && !errors.Is(serveErr, http.ErrServerClosed) {
			log.Fatalf("serve HTTP: %v", serveErr)
		}
	case <-ctx.Done():
		shutdownCtx, cancel := context.WithTimeout(context.Background(), 5*time.Second)
		defer cancel()
		if shutdownErr := server.Shutdown(shutdownCtx); shutdownErr != nil {
			log.Printf("shutdown HTTP server: %v", shutdownErr)
		}
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

func envBoolOrDefault(name string, fallback bool) (bool, error) {
	value := os.Getenv(name)
	if value == "" {
		return fallback, nil
	}
	parsed, err := strconv.ParseBool(value)
	if err != nil {
		return false, fmt.Errorf("%s must be true or false: %q", name, value)
	}
	return parsed, nil
}
