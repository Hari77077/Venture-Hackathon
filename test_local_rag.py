import os
import time
import chromadb
from chromadb.utils.embedding_functions import (
    SentenceTransformerEmbeddingFunction,
)

# -------------------------------------------------------------
# Configuration
# -------------------------------------------------------------
DB_PATH = "./db"
COLLECTION_NAME = "disaster_frameworks"
EMBED_MODEL = "sentence-transformers/all-MiniLM-L6-v2"

# -------------------------------------------------------------
# 1. Initialize Persistent Client on CPU
# -------------------------------------------------------------
print("Connecting to ChromaDB and initializing embedding model on CPU...")
start_init = time.perf_counter()

embed_fn = SentenceTransformerEmbeddingFunction(
    model_name=EMBED_MODEL,
    device="cpu",  # Direct CPU execution (no CUDA dependencies needed)
)

client = chromadb.PersistentClient(path=DB_PATH)

try:
    collection = client.get_collection(
        name=COLLECTION_NAME, embedding_function=embed_fn
    )
    print(
        f"Database connected in {time.perf_counter() - start_init:.2f}s."
        f" Total indexed records: {collection.count()}"
    )
except Exception as e:
    print(f"Failed to find collection '{COLLECTION_NAME}': {e}")
    exit(1)


# -------------------------------------------------------------
# 2. Retrieval & Benchmark Function
# -------------------------------------------------------------
def query_disaster_db(query: str, top_k: int = 2):
    print(f"\nEvaluating Query: '{query}'")
    t0 = time.perf_counter()

    results = collection.query(query_texts=[query], n_results=top_k)

    latency_ms = (time.perf_counter() - t0) * 1000.0
    print(
        f"Retrieval complete in {latency_ms:.2f} ms ({top_k} nearest matches)\n"
    )

    docs = results["documents"][0]
    metas = results["metadatas"][0]
    distances = results["distances"][0]

    for idx, (doc, meta, dist) in enumerate(zip(docs, metas, distances), 1):
        print(f"--- [Match {idx}] ---")
        print(
            f"Source   : {meta.get('source')} (Page {meta.get('page')}) |"
            f" Cosine Dist: {dist:.4f}"
        )
        print(f"Content  :\n{doc.strip()}\n")


# -------------------------------------------------------------
# 3. Test Execution
# -------------------------------------------------------------
if __name__ == "__main__":
    test_query = (
        "What are the specific duties and responsibilities of the Incident"
        " Commander?"
    )
    query_disaster_db(test_query, top_k=2)