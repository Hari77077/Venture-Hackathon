package llm_client

import (
	"bytes"
	"encoding/json"
	"fmt"
	"io"
	"net/http"
	"time"
)

// Config holds connection details for both Qwen and ChromaDB.
type Config struct {
	QwenURL    string // e.g. "http://localhost:11434/api/chat" (Ollama)
	QwenModel  string // "qwen2.5:1.5b"
	ChromaURL  string // e.g. "http://localhost:8000"
	Collection string // ChromaDB collection name
}

// DefaultConfig returns sane local-dev defaults (Ollama + Chroma).
func DefaultConfig() Config {
	return Config{
		QwenURL:    "http://localhost:11434/api/chat",
		QwenModel:  "qwen2.5:1.5b",
		ChromaURL:  "http://localhost:8000",
		Collection: "flood_advisories",
	}
}

// ---------- ChromaDB query ----------

type chromaQueryReq struct {
	QueryTexts []string `json:"query_texts"`
	NResults   int      `json:"n_results"`
}

type chromaQueryResp struct {
	Documents [][]string `json:"documents"`
}

// QueryChroma retrieves the top-k most relevant documents from ChromaDB.
func QueryChroma(cfg Config, query string, topK int) ([]string, error) {
	url := fmt.Sprintf("%s/api/v1/collections/%s/query", cfg.ChromaURL, cfg.Collection)

	body, _ := json.Marshal(chromaQueryReq{
		QueryTexts: []string{query},
		NResults:   topK,
	})

	resp, err := http.Post(url, "application/json", bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("chroma query failed: %w", err)
	}
	defer resp.Body.Close()

	var result chromaQueryResp
	if err := json.NewDecoder(resp.Body).Decode(&result); err != nil {
		return nil, fmt.Errorf("chroma decode failed: %w", err)
	}

	if len(result.Documents) > 0 {
		return result.Documents[0], nil
	}
	return nil, nil
}

// ---------- Qwen chat completion (Ollama-compatible) ----------

type ollamaMessage struct {
	Role    string `json:"role"`
	Content string `json:"content"`
}

type ollamaChatReq struct {
	Model    string          `json:"model"`
	Messages []ollamaMessage `json:"messages"`
	Stream   bool            `json:"stream"`
}

type ollamaChatResp struct {
	Message ollamaMessage `json:"message"`
}

// GenerateAdvisory builds a RAG prompt from ChromaDB context + sensor data,
// then queries Qwen 2.5 for a plain-text flood/landslide advisory.
func GenerateAdvisory(cfg Config, sensorJSON string) (string, error) {
	// 1. Retrieve relevant context from ChromaDB
	docs, err := QueryChroma(cfg, sensorJSON, 3)
	if err != nil {
		return "", err
	}

	context := ""
	for _, d := range docs {
		context += d + "\n"
	}

	// 2. Build the prompt
	systemPrompt := `You are a disaster-management AI advisor for Kerala, India.
Given the sensor telemetry and retrieved context below, produce a concise,
actionable advisory: severity level, affected areas, and recommended actions.
Keep it under 200 words.`

	userPrompt := fmt.Sprintf("## Retrieved Context\n%s\n## Live Sensor Data\n%s",
		context, sensorJSON)

	// 3. Call Qwen via Ollama
	payload, _ := json.Marshal(ollamaChatReq{
		Model: cfg.QwenModel,
		Messages: []ollamaMessage{
			{Role: "system", Content: systemPrompt},
			{Role: "user", Content: userPrompt},
		},
		Stream: false,
	})

	client := &http.Client{Timeout: 60 * time.Second}
	resp, err := client.Post(cfg.QwenURL, "application/json", bytes.NewReader(payload))
	if err != nil {
		return "", fmt.Errorf("qwen request failed: %w", err)
	}
	defer resp.Body.Close()

	raw, _ := io.ReadAll(resp.Body)
	var result ollamaChatResp
	if err := json.Unmarshal(raw, &result); err != nil {
		return "", fmt.Errorf("qwen decode failed: %w", err)
	}

	return result.Message.Content, nil
}
