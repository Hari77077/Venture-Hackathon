import pandas as pd
import numpy as np
import os

def generate_synthetic_data(output_dir):
    os.makedirs(output_dir, exist_ok=True)
    
    # Generate 1 year of hourly data (365 * 24 = 8760 hours)
    dates = pd.date_range(start="2021-01-01", periods=8760, freq="H")
    
    # Synthetic Rainfall (mostly 0, some random spikes)
    # Using a pareto distribution to simulate dry spells and heavy rain
    rainfall = np.random.pareto(a=5, size=8760) * 2 
    rainfall[rainfall < 1.0] = 0.0 # Make it 0 for most days
    
    # Synthetic Water Level (base level 2.0m, increases with rainfall)
    # Simple rolling sum of rainfall to simulate water level rising
    water_level = 2.0 + pd.Series(rainfall).rolling(window=48, min_periods=1).mean() * 0.5
    # Add some noise
    water_level += np.random.normal(0, 0.1, 8760)
    
    # Synthetic Pressure (base 1010 hPa, drops when rainfall is high)
    pressure = 1010.0 - pd.Series(rainfall).rolling(window=12, min_periods=1).mean() * 0.2
    
    # Create DataFrames
    rain_df = pd.DataFrame({
        "date": dates,
        "rainfall_mm": rainfall,
        "pressure_hpa": pressure
    })
    
    wl_df = pd.DataFrame({
        "date": dates,
        "water_level_m": water_level
    })
    
    # Save to CSV
    rain_path = os.path.join(output_dir, "imd_rainfall_kottayam.csv")
    wl_path = os.path.join(output_dir, "cwc_waterlevel_kottayam.csv")
    
    rain_df.to_csv(rain_path, index=False)
    wl_df.to_csv(wl_path, index=False)
    
    print(f"Synthetic data generated at {output_dir}")
    print(f"- {rain_path}")
    print(f"- {wl_path}")

if __name__ == "__main__":
    generate_synthetic_data("datasets/processed")
