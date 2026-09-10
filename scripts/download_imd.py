import os
import argparse
import imdlib as imd

def download_imd_data(start_yr, end_yr, var_type='rain', output_dir='../datasets/imd_gridded'):
    """
    Downloads IMD gridded data for a specified year range.
    var_type can be 'rain', 'tmax', 'tmin'.
    """
    if not os.path.exists(output_dir):
        os.makedirs(output_dir)
        
    print(f"Downloading IMD {var_type} data from {start_yr} to {end_yr}...")
    
    # Download data to the output directory
    data = imd.get_data(var_type, start_yr, end_yr, fn_format='yearwise', file_dir=output_dir)
    
    print(f"Download complete. Files saved in {output_dir}")
    
    # Normally, imdlib saves as .grd files. We can also attempt to convert to CSV or NetCDF.
    # For now, getting the raw grids is the first step.
    
if __name__ == '__main__':
    parser = argparse.ArgumentParser(description="Download IMD Gridded Rainfall Data")
    parser.add_argument('--start', type=int, default=2018, help="Start year (e.g. 1901)")
    parser.add_argument('--end', type=int, default=2022, help="End year (e.g. 2022)")
    parser.add_argument('--var', type=str, default='rain', choices=['rain', 'tmax', 'tmin'], help="Variable to download")
    parser.add_argument('--out_dir', type=str, default='../datasets/imd_gridded', help="Output directory")
    
    args = parser.parse_args()
    
    # Resolve absolute path relative to this script
    script_dir = os.path.dirname(os.path.abspath(__file__))
    out_path = os.path.normpath(os.path.join(script_dir, args.out_dir))
    
    download_imd_data(args.start, args.end, args.var, out_path)
