"""
Lightweight FastAPI inference server for the dual-head flood LSTM.

The Go backend POSTs sensor + weather JSON here and gets back:
  - predicted water level
  - flood probability

Run:  MODEL_PATH=models/model_kottayam.pt uvicorn ml.inference_server:app --port 5000
"""
import os

import numpy as np
import torch
import torch.nn as nn
from fastapi import FastAPI, HTTPException
from pydantic import BaseModel


class FloodLSTM(nn.Module):
    def __init__(self, input_dim=4, hidden_dim=32, num_layers=2, dropout=0.2):
        super().__init__()
        self.lstm = nn.LSTM(
            input_dim, hidden_dim, num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0.0,
        )
        self.reg_head = nn.Linear(hidden_dim, 1)
        self.cls_head = nn.Sequential(
            nn.Linear(hidden_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 1),
            nn.Sigmoid(),
        )

    def forward(self, x):
        out, _ = self.lstm(x)
        h = out[:, -1, :]
        return self.reg_head(h), self.cls_head(h)


app = FastAPI(title="Flood Inference API")

_model = None
_meta = None


class PredictRequest(BaseModel):
    """2-D list: [[rainfall_mm, rate_of_rise, antecedent_rain, pressure_trend], ...]
    with shape (seq_len, 4)."""
    sequence: list[list[float]]


class PredictResponse(BaseModel):
    region: str
    predicted_water_level: float
    flood_probability: float
    flood_threshold_m: float


@app.on_event("startup")
def load_model():
    global _model, _meta
    path = os.environ.get("MODEL_PATH", "models/model.pt")
    if not os.path.exists(path):
        raise RuntimeError(f"Model not found: {path}")

    ckpt = torch.load(path, map_location="cpu", weights_only=False)
    _meta = ckpt
    _model = FloodLSTM(
        input_dim=ckpt["input_dim"],
        hidden_dim=ckpt["hidden_dim"],
        num_layers=ckpt["num_layers"],
    )
    _model.load_state_dict(ckpt["state_dict"])
    _model.eval()


@app.get("/health")
def health():
    return {"status": "ok", "region": _meta["region"] if _meta else "not loaded"}


@app.post("/predict", response_model=PredictResponse)
def predict(req: PredictRequest):
    if _model is None:
        raise HTTPException(503, "Model not loaded")

    arr = np.array([req.sequence], dtype=np.float32)

    # Apply same normalization used during training
    mean = np.array(_meta["norm_mean"], dtype=np.float32)
    std = np.array(_meta["norm_std"], dtype=np.float32)
    arr = (arr - mean) / std

    tensor = torch.tensor(arr)
    with torch.no_grad():
        wl, fl = _model(tensor)

    return PredictResponse(
        region=_meta["region"],
        predicted_water_level=round(wl.item(), 4),
        flood_probability=round(fl.item(), 4),
        flood_threshold_m=_meta["flood_threshold_m"],
    )
