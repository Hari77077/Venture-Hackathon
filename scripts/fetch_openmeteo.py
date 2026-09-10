import requests
import argparse
import pandas as pd
import os

def fetch_historical_weather(lat, lon, start_date, end_date, output_csv):
    """
    Fetches historical daily precipitation data from Open-Meteo for a given location.
    """
    url = "https://archive-api.open-meteo.com/v1/archive"
    
    params = {
        "latitude": lat,
        "longitude": lon,
        "start_date": start_date,
        "end_date": end_date,
        "daily": "precipitation_sum",
        "timezone": "auto"
    }
    
    print(f"Fetching Open-Meteo data for Lat: {lat}, Lon: {lon} from {start_date} to {end_date}...")
    response = requests.get(url, params=params)
    
    if response.status_code == 200:
        data = response.json()
        
        # Extract daily data
        if 'daily' in data:
            df = pd.DataFrame({
                'date': data['daily']['time'],
                'precipitation_sum_mm': data['daily']['precipitation_sum']
            })
            
            # Save to CSV
            os.makedirs(os.path.dirname(os.path.abspath(output_csv)), exist_ok=True)
            df.to_csv(output_csv, index=False)
            print(f"Successfully saved to {output_csv}")
        else:
            print("No daily data found in the response.")
    else:
        print(f"Failed to fetch data. Status Code: {response.status_code}")
        print(response.text)

if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Fetch historical data from Open-Meteo")
    # Default coordinates for Kerala (approximate center)
    parser.add_argument('--lat', type=float, default=10.8505, help="Latitude")
    parser.add_argument('--lon', type=float, default=76.2711, help="Longitude")
    parser.add_argument('--start', type=str, default="2018-08-01", help="Start date YYYY-MM-DD")
    parser.add_argument('--end', type=str, default="2018-08-31", help="End date YYYY-MM-DD")
    parser.add_argument('--out', type=str, default="../datasets/openmeteo_kerala_2018.csv", help="Output CSV path")
    
    args = parser.parse_args()
    
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(script_dir, args.out))
    
    fetch_historical_weather(args.lat, args.lon, args.start, args.end, out_path)
