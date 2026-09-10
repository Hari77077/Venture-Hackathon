"""
train.py — DisasterMesh flood-level LSTM training script

Trains a dual-head LSTM:
  Head 1 (regression):     predicts water level HORIZON_HOURS ahead
  Head 2 (classification): predicts flood / no-flood binary label

Usage:
    python train.py --region kottayam \
        --rainfall data/imd_rainfall_kottayam.csv \
        --waterlevel data/cwc_waterlevel_kottayam.csv \
        --epochs 100

Expected CSV formats:
    rainfall CSV:   date, rainfall_mm  (optionally: pressure_hpa)
    waterlevel CSV: date, water_level_m
    Both must have hourly or daily timestamps covering the same period.

If a `pressure_hpa` column exists in the rainfall CSV, the script
derives a pressure_trend feature automatically. Otherwise it's zeroed
(documented limitation: IMD/CWC historical data lacks pressure; it
becomes a live feature once Sentinel Node BMP180 telemetry flows).
"""

import argparse
import os

import numpy as np
import pandas as pd
import torch
import torch.nn as nn
from torch.utils.data import Dataset, DataLoader

SEQ_LEN = 24            # hours of lookback per prediction
HORIZON_HOURS = 6       # how far ahead we predict
ANTECEDENT_WINDOW = 72  # rolling rain sum window (soil saturation proxy)
FLOOD_THRESHOLD_M = 4.5 # water level above this = flood label = 1


# ---------------------------------------------------------------------
# Feature engineering
# ---------------------------------------------------------------------

def build_features(rainfall_csv: str, waterlevel_csv: str) -> pd.DataFrame:
    rain = pd.read_csv(rainfall_csv, parse_dates=["date"])
    level = pd.read_csv(waterlevel_csv, parse_dates=["date"])

    df = pd.merge(rain, level, on="date", how="inner").sort_values("date")
    df = df.set_index("date")

    # Rate of rise: Δ water level per timestep
    df["rate_of_rise"] = df["water_level_m"].diff().fillna(0.0)

    # Antecedent rainfall: rolling sum (soil saturation proxy)
    df["antecedent_rain"] = (
        df["rainfall_mm"].rolling(ANTECEDENT_WINDOW, min_periods=1).sum()
    )

    # Pressure trend (only if column exists from BMP180 backfill)
    if "pressure_hpa" in df.columns:
        df["pressure_trend"] = df["pressure_hpa"].diff().fillna(0.0)
    else:
        df["pressure_trend"] = 0.0

    # Flood binary label (for classification head)
    df["flood_label"] = (df["water_level_m"] >= FLOOD_THRESHOLD_M).astype(np.float32)

    df = df.dropna(subset=["water_level_m", "rainfall_mm"])

    return df


FEATURE_COLS = ["rainfall_mm", "rate_of_rise", "antecedent_rain", "pressure_trend"]


def make_sequences(df: pd.DataFrame):
    """Slide a SEQ_LEN window; target is water_level_m and flood_label
    at HORIZON_HOURS after window end."""
    values = df[FEATURE_COLS].values.astype(np.float32)
    wl = df["water_level_m"].values.astype(np.float32)
    fl = df["flood_label"].values.astype(np.float32)

    X, y_reg, y_cls = [], [], []
    for i in range(len(df) - SEQ_LEN - HORIZON_HOURS):
        X.append(values[i : i + SEQ_LEN])
        target_idx = i + SEQ_LEN + HORIZON_HOURS
        y_reg.append(wl[target_idx])
        y_cls.append(fl[target_idx])

    return np.array(X), np.array(y_reg), np.array(y_cls)


class FloodDataset(Dataset):
    def __init__(self, X, y_reg, y_cls):
        self.X = torch.tensor(X, dtype=torch.float32)
        self.y_reg = torch.tensor(y_reg, dtype=torch.float32).unsqueeze(-1)
        self.y_cls = torch.tensor(y_cls, dtype=torch.float32).unsqueeze(-1)

    def __len__(self):
        return len(self.X)

    def __getitem__(self, idx):
        return self.X[idx], self.y_reg[idx], self.y_cls[idx]


# ---------------------------------------------------------------------
# Dual-head model
# ---------------------------------------------------------------------

class FloodLSTM(nn.Module):
    """
    Shared LSTM backbone with two heads:
      - regression:     predicts water_level_m (MSE loss)
      - classification: predicts flood probability (BCE loss)
    """

    def __init__(self, input_dim=4, hidden_dim=32, num_layers=2, dropout=0.2):
        super().__init__()
        self.lstm = nn.LSTM(
            input_dim, hidden_dim, num_layers,
            batch_first=True,
            dropout=dropout if num_layers > 1 else 0.0,
        )

        # Regression head → predicted water level
        self.reg_head = nn.Linear(hidden_dim, 1)

        # Classification head → flood probability
        self.cls_head = nn.Sequential(
            nn.Linear(hidden_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 1),
            nn.Sigmoid(),
        )

    def forward(self, x):
        out, _ = self.lstm(x)
        h = out[:, -1, :]            # last timestep hidden state
        wl_pred = self.reg_head(h)   # (batch, 1)
        flood_prob = self.cls_head(h) # (batch, 1)
        return wl_pred, flood_prob


# ---------------------------------------------------------------------
# Normalization helpers
# ---------------------------------------------------------------------

def normalize(X, mean=None, std=None):
    if mean is None:
        mean = X.reshape(-1, X.shape[-1]).mean(axis=0)
        std = X.reshape(-1, X.shape[-1]).std(axis=0) + 1e-8
    return (X - mean) / std, mean, std


# ---------------------------------------------------------------------
# Training loop
# ---------------------------------------------------------------------

def train(args):
    df = build_features(args.rainfall, args.waterlevel)
    X, y_reg, y_cls = make_sequences(df)

    print(f"[{args.region}] samples={len(X)}  features={X.shape[-1]}  "
          f"flood_ratio={y_cls.mean():.3f}")

    # Time-based split — never shuffle time series splits
    split = int(len(X) * 0.85)
    X_train, X_val = X[:split], X[split:]
    yr_train, yr_val = y_reg[:split], y_reg[split:]
    yc_train, yc_val = y_cls[:split], y_cls[split:]

    X_train, feat_mean, feat_std = normalize(X_train)
    X_val, _, _ = normalize(X_val, feat_mean, feat_std)

    train_dl = DataLoader(
        FloodDataset(X_train, yr_train, yc_train),
        batch_size=args.batch_size, shuffle=True,
    )
    val_dl = DataLoader(
        FloodDataset(X_val, yr_val, yc_val),
        batch_size=args.batch_size, shuffle=False,
    )

    model = FloodLSTM(
        input_dim=X.shape[-1],
        hidden_dim=args.hidden_dim,
        num_layers=args.num_layers,
    )
    optimizer = torch.optim.Adam(model.parameters(), lr=args.lr)
    scheduler = torch.optim.lr_scheduler.StepLR(optimizer, step_size=15, gamma=0.5)
    mse_fn = nn.MSELoss()
    bce_fn = nn.BCELoss()

    best_val = float("inf")
    patience, wait = 8, 0
    out_dir = os.path.dirname(args.out) or "models"
    os.makedirs(out_dir, exist_ok=True)
    best_path = os.path.join(out_dir, f"best_{args.region}.pt")

    for epoch in range(1, args.epochs + 1):
        # --- train ---
        model.train()
        t_loss = 0.0
        for xb, yb_r, yb_c in train_dl:
            optimizer.zero_grad()
            pred_wl, pred_fl = model(xb)
            loss = mse_fn(pred_wl, yb_r) + bce_fn(pred_fl, yb_c)
            loss.backward()
            optimizer.step()
            t_loss += loss.item() * xb.size(0)
        scheduler.step()
        t_loss /= len(train_dl.dataset)

        # --- validate ---
        model.eval()
        v_loss = 0.0
        with torch.no_grad():
            for xb, yb_r, yb_c in val_dl:
                pred_wl, pred_fl = model(xb)
                v_loss += (mse_fn(pred_wl, yb_r) + bce_fn(pred_fl, yb_c)).item() * xb.size(0)
        v_loss /= len(val_dl.dataset)

        if epoch % 5 == 0 or epoch == 1:
            print(f"  epoch {epoch:03d}/{args.epochs}  "
                  f"train={t_loss:.4f}  val={v_loss:.4f}")

        if v_loss < best_val:
            best_val = v_loss
            wait = 0
            torch.save(model.state_dict(), best_path)
        else:
            wait += 1
            if wait >= patience:
                print(f"  early stop at epoch {epoch}")
                break

    # --- save final checkpoint with all metadata ---
    model.load_state_dict(torch.load(best_path, weights_only=True))
    model.eval()

    save_path = args.out
    torch.save({
        "region": args.region,
        "state_dict": model.state_dict(),
        "input_dim": X.shape[-1],
        "hidden_dim": args.hidden_dim,
        "num_layers": args.num_layers,
        "seq_len": SEQ_LEN,
        "horizon": HORIZON_HOURS,
        "flood_threshold_m": FLOOD_THRESHOLD_M,
        "feature_cols": FEATURE_COLS,
        "norm_mean": feat_mean.tolist(),
        "norm_std": feat_std.tolist(),
        "best_val_loss": best_val,
    }, save_path)

    print(f"\nSaved -> {save_path}  |  best_val={best_val:.4f}")
    print(f"  features: {FEATURE_COLS}")
    print(f"  norm stats embedded in checkpoint (no separate .npz needed)")


# ---------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------

if __name__ == "__main__":
    p = argparse.ArgumentParser(description="DisasterMesh zone-based flood LSTM trainer")
    p.add_argument("--region",     required=True, help="Zone name (e.g. kottayam)")
    p.add_argument("--rainfall",   required=True, help="Rainfall CSV (date, rainfall_mm)")
    p.add_argument("--waterlevel", required=True, help="Water-level CSV (date, water_level_m)")
    p.add_argument("--out",        default="models/model.pt", help="Output .pt path")
    p.add_argument("--epochs",     type=int,   default=100)
    p.add_argument("--batch-size", type=int,   default=32)
    p.add_argument("--hidden-dim", type=int,   default=32)
    p.add_argument("--num-layers", type=int,   default=2)
    p.add_argument("--lr",         type=float, default=1e-3)
    p.add_argument("--flood-threshold", type=float, default=4.5,
                   help="Water level (m) above which = flood")
    args = p.parse_args()

    FLOOD_THRESHOLD_M = args.flood_threshold
    train(args)
