package main

import (
	"encoding/json"
	"fmt"
	"io"
	"log"
	"net/http"
	"sync"
	"time"
)

// ---------- Weather cache (Open-Meteo) ----------

type WeatherData struct {
	Temperature   float64 `json:"temperature_2m"`
	Precipitation float64 `json:"precipitation"`
	FetchedAt     string  `json:"fetched_at"`
}

var (
	weatherMu    sync.RWMutex
	latestWeather *WeatherData
)

// fetchWeather polls Open-Meteo for Kerala (lat=10.85, lon=76.27).
func fetchWeather() {
	url := "https://api.open-meteo.com/v1/forecast?latitude=10.85&longitude=76.27&current=temperature_2m,precipitation"

	resp, err := http.Get(url)
	if err != nil {
		log.Printf("[weather] fetch error: %v", err)
		return
	}
	defer resp.Body.Close()

	var raw struct {
		Current struct {
			Temp   float64 `json:"temperature_2m"`
			Precip float64 `json:"precipitation"`
		} `json:"current"`
	}
	if err := json.NewDecoder(resp.Body).Decode(&raw); err != nil {
		log.Printf("[weather] decode error: %v", err)
		return
	}

	weatherMu.Lock()
	latestWeather = &WeatherData{
		Temperature:   raw.Current.Temp,
		Precipitation: raw.Current.Precip,
		FetchedAt:     time.Now().Format(time.RFC3339),
	}
	weatherMu.Unlock()
	log.Printf("[weather] updated: %.1f°C, %.1fmm precip", raw.Current.Temp, raw.Current.Precip)
}

func weatherLoop(interval time.Duration) {
	fetchWeather() // immediate first fetch
	ticker := time.NewTicker(interval)
	for range ticker.C {
		fetchWeather()
	}
}

// ---------- Handlers ----------

func healthHandler(w http.ResponseWriter, r *http.Request) {
	w.Header().Set("Content-Type", "application/json")
	w.WriteHeader(http.StatusOK)
	w.Write([]byte(`{"status":"ok"}`))
}

func rootHandler(w http.ResponseWriter, r *http.Request) {
	fmt.Fprintf(w, "Server is operational at %s", time.Now().Format(time.RFC3339))
}

func weatherHandler(w http.ResponseWriter, r *http.Request) {
	weatherMu.RLock()
	defer weatherMu.RUnlock()

	w.Header().Set("Content-Type", "application/json")
	if latestWeather == nil {
		http.Error(w, `{"error":"no weather data yet"}`, http.StatusServiceUnavailable)
		return
	}
	json.NewEncoder(w).Encode(latestWeather)
}

// ingestHandler receives JSON telemetry from mesh gateway nodes.
func ingestHandler(w http.ResponseWriter, r *http.Request) {
	if r.Method != http.MethodPost {
		http.Error(w, "POST only", http.StatusMethodNotAllowed)
		return
	}

	body, err := io.ReadAll(r.Body)
	if err != nil {
		http.Error(w, "bad body", http.StatusBadRequest)
		return
	}
	defer r.Body.Close()

	// For now, log the payload. In production: write to DB + trigger inference.
	log.Printf("[ingest] received %d bytes: %s", len(body), string(body))

	w.Header().Set("Content-Type", "application/json")
	w.Write([]byte(`{"status":"ingested"}`))
}

// ---------- Main ----------

func main() {
	mux := http.NewServeMux()
	mux.HandleFunc("/health", healthHandler)
	mux.HandleFunc("/weather", weatherHandler)
	mux.HandleFunc("/ingest", ingestHandler)
	mux.HandleFunc("/", rootHandler)

	// Start weather polling every 30 minutes
	go weatherLoop(30 * time.Minute)

	server := &http.Server{
		Addr:         ":8080",
		Handler:      mux,
		ReadTimeout:  5 * time.Second,
		WriteTimeout: 10 * time.Second,
		IdleTimeout:  15 * time.Second,
	}

	log.Printf("Server listening on http://localhost%s", server.Addr)
	log.Fatalf("Server failed: %v", server.ListenAndServe())
}

