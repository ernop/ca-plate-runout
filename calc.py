#!/usr/bin/env python3
"""
California License Plate Sequence Analysis & Prediction

Analyzes observed license plate issuance dates to predict future milestones
including series start/end dates and the final 9ZZZ999 plate.
"""

# Use non-interactive backend - MUST be set before importing pyplot
# This prevents blocking popups and allows continuous iteration
import matplotlib
matplotlib.use('Agg')

import numpy as np
import matplotlib.pyplot as plt
import matplotlib.dates as mdates
from matplotlib.gridspec import GridSpec
from datetime import datetime, timedelta
from collections import defaultdict
from pathlib import Path

# =============================================================================
# DATA
# =============================================================================

data = [
    # Extended data (2025 newer plates)
    ('9WXZ302', '12/30/25'), ('9WXS727', '12/27/25'), ('9WXK703', '12/22/25'),
    ('9WRL223', '12/21/25'), ('9WRK453', '12/21/25'), ('9WRC031', '12/20/25'),
    ('9WQX442', '12/19/25'), ('9WQT658', '12/17/25'), ('9WOZ197', '12/12/25'),
    ('9WNK105', '12/08/25'), ('9WMY388', '12/08/25'), ('9WMA185', '12/07/25'),
    ('9WLY391', '12/03/25'), ('9WIV444', '12/02/25'), ('9WDR048', '12/02/25'),
    ('9WIC011', '12/01/25'), ('9WCR501', '12/01/25'), ('9WLM382', '10/28/25'),
    ('9WIY021', '10/17/25'), ('9VZN747', '10/06/25'), ('9VZJ632', '10/05/25'),
    ('9VUB832', '10/01/25'), ('9VTH090', '9/23/25'), ('9VQY040', '9/20/25'),
    ('9VLE466', '9/19/25'), ('9VJY731', '9/16/25'), ('9VJA169', '9/13/25'),
    ('9VHV988', '9/09/25'), ('9VGA297', '8/31/25'), ('9VGZ301', '8/30/25'),
    ('9VFY541', '8/24/25'), ('9VCJ356', '8/14/25'), ('9UYY443', '8/07/25'),
    ('9UVN347', '7/23/25'), ('9UUV150', '7/23/25'), ('9URZ698', '7/22/25'),
    ('9UQP242', '7/21/25'), ('9UPA824', '7/20/25'), ('9UNJ672', '7/12/25'),
    ('9UMY508', '7/11/25'), ('9UMY078', '7/06/25'), ('9ULY079', '6/30/25'),
    ('9UKC300', '6/22/25'), ('9UHK320', '6/21/25'), ('9UHA269', '6/20/25'),
    ('9UDB728', '6/18/25'), ('9UCF154', '6/15/25'), ('9UBZ506', '6/14/25'),
    ('9UBY762', '6/09/25'), ('9UAB021', '6/07/25'), ('9UAY436', '6/06/25'),
    ('9TZZ735', '5/27/25'), ('9TZY047', '5/20/25'), ('9TYC454', '5/15/25'),
    ('9TWB028', '5/12/25'), ('9TYB181', '5/12/25'), ('9TVR562', '5/02/25'),
    ('9TTB467', '4/20/25'), ('9TQS519', '4/19/25'), ('9THS267', '4/05/25'),
    ('9THS025', '4/04/25'), ('9TCY474', '4/04/25'), ('9SZX599', '3/26/25'),
    ('9SZL656', '3/19/25'),
    # Original data (2024-2025 older plates)
    ('9STD057', '2/27/25'), ('9SRT999', '2/17/25'), ('9SFY812', '1/28/25'),
    ('9SBX339', '1/23/25'), ('9RYA062', '1/22/25'), ('9RXY106', '1/20/25'),
    ('9RXG166', '1/17/25'), ('9RWS099', '1/12/25'), ('9RVP374', '1/08/25'),
    ('9RTU099', '1/01/25'), ('9RSX173', '12/31/24'), ('9RTG701', '12/27/24'),
    ('9RTB567', '12/26/24'), ('9RTA730', '12/22/24'), ('9RTA550', '12/21/24'),
    ('9RSW611', '12/20/24'), ('9RSU010', '12/04/24'), ('9RIW652', '12/02/24'),
    ('9RIW078', '11/30/24'), ('9RIK209', '11/28/24'), ('9RBT282', '11/14/24'),
    ('9RBA110', '11/13/24'), ('9PZY368', '11/09/24'), ('9PVZ831', '10/28/24'),
    ('9PVY438', '10/26/24'), ('9PVY700', '10/25/24'), ('9PRV673', '10/24/24'),
    ('9PRE159', '10/24/24'), ('9PRF033', '10/23/24'), ('9PQX157', '10/19/24'),
    ('9PRZ051', '10/19/24'), ('9PQX059', '10/09/24'), ('9PJV472', '10/08/24')
]


# =============================================================================
# PLATE ENCODING/DECODING
# =============================================================================

LETTERS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"

def plate_to_num(plate: str) -> int:
    """
    Convert plate string to sequential number.
    Format: 9ABC123 where ABC are letters and 123 is numeric suffix.
    
    Sequence: 9AAA000 = 0, 9AAA001 = 1, ..., 9AAA999 = 999,
              9AAB000 = 1000, ..., 9ZZZ999 = max
    """
    l1 = LETTERS.index(plate[1])  # First letter (A-Z)
    l2 = LETTERS.index(plate[2])  # Second letter
    l3 = LETTERS.index(plate[3])  # Third letter
    num_suffix = int(plate[4:7])  # Numeric suffix (000-999)
    
    return (l1 * 26 * 26 + l2 * 26 + l3) * 1000 + num_suffix


def num_to_plate(num: int) -> str:
    """Convert sequential number back to plate string."""
    num_suffix = num % 1000
    letter_part = num // 1000
    
    l3 = letter_part % 26
    l2 = (letter_part // 26) % 26
    l1 = (letter_part // (26 * 26)) % 26
    
    return f"9{LETTERS[l1]}{LETTERS[l2]}{LETTERS[l3]}{num_suffix:03d}"


def get_series_letter(plate: str) -> str:
    """Extract the series letter (first letter after 9)."""
    return plate[1]


def get_unique_filename(base_name: str, extension: str = '.png') -> str:
    """
    Generate a unique filename by appending a number if file already exists.
    e.g., plate_analysis.png -> plate_analysis_1.png -> plate_analysis_2.png
    """
    path = Path(base_name + extension)
    if not path.exists():
        return str(path)
    
    counter = 1
    while True:
        new_path = Path(f"{base_name}_{counter}{extension}")
        if not new_path.exists():
            return str(new_path)
        counter += 1


# =============================================================================
# REGRESSION & STATISTICS
# =============================================================================

def linear_regression(x: np.ndarray, y: np.ndarray) -> tuple:
    """
    Manual linear regression using least squares.
    Returns (slope, intercept, r_squared, std_error).
    """
    n = len(x)
    x_mean = np.mean(x)
    y_mean = np.mean(y)
    
    # Slope and intercept
    numerator = np.sum((x - x_mean) * (y - y_mean))
    denominator = np.sum((x - x_mean) ** 2)
    slope = numerator / denominator
    intercept = y_mean - slope * x_mean
    
    # R-squared
    y_pred = slope * x + intercept
    ss_res = np.sum((y - y_pred) ** 2)
    ss_tot = np.sum((y - y_mean) ** 2)
    r_squared = 1 - (ss_res / ss_tot)
    
    # Standard error of the estimate
    std_error = np.sqrt(ss_res / (n - 2))
    
    return slope, intercept, r_squared, std_error


def predict_date(plate_num: int, slope: float, intercept: float) -> datetime:
    """Predict date when a plate number will be issued."""
    ordinal = (plate_num - intercept) / slope
    return datetime.fromordinal(int(ordinal))


def predict_plate(date: datetime, slope: float, intercept: float) -> int:
    """Predict plate number that will be issued on a given date."""
    return int(slope * date.toordinal() + intercept)


# =============================================================================
# MILESTONE CALCULATIONS
# =============================================================================

def calculate_milestones(slope: float, intercept: float) -> dict:
    """
    Calculate key milestone dates for the 9-series plates.
    Each letter series contains 26 * 26 * 1000 = 676,000 plates.
    """
    milestones = {}
    plates_per_series = 26 * 26 * 1000  # 676,000
    
    # Series boundaries (9X, 9Y, 9Z)
    series_letters = ['P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z']
    
    for letter in series_letters:
        idx = LETTERS.index(letter)
        start_num = idx * plates_per_series
        end_num = (idx + 1) * plates_per_series - 1
        
        start_plate = num_to_plate(start_num)
        end_plate = num_to_plate(end_num)
        
        milestones[f'9{letter}'] = {
            'start_plate': start_plate,
            'end_plate': end_plate,
            'start_num': start_num,
            'end_num': end_num,
            'start_date': predict_date(start_num, slope, intercept),
            'end_date': predict_date(end_num, slope, intercept),
        }
    
    # Special milestone: 9ZZZ999 (the final plate)
    final_num = plate_to_num('9ZZZ999')
    milestones['FINAL'] = {
        'plate': '9ZZZ999',
        'num': final_num,
        'date': predict_date(final_num, slope, intercept),
    }
    
    return milestones


# =============================================================================
# ANALYSIS
# =============================================================================

def analyze_data(data: list) -> dict:
    """Comprehensive analysis of the plate data."""
    
    # Parse data
    plates = []
    dates = []
    for plate, date_str in data:
        plates.append(plate)
        dates.append(datetime.strptime(date_str, '%m/%d/%y'))
    
    # Convert to numpy arrays
    plate_nums = np.array([plate_to_num(p) for p in plates])
    date_ordinals = np.array([d.toordinal() for d in dates])
    
    # Sort by date
    sort_idx = np.argsort(date_ordinals)
    plate_nums_sorted = plate_nums[sort_idx]
    date_ordinals_sorted = date_ordinals[sort_idx]
    dates_sorted = [dates[i] for i in sort_idx]
    plates_sorted = [plates[i] for i in sort_idx]
    
    # Linear regression
    slope, intercept, r_squared, std_error = linear_regression(date_ordinals_sorted, plate_nums_sorted)
    
    # Calculate daily issuance rate
    plates_per_day = slope
    
    # Time span
    date_range = max(dates) - min(dates)
    plate_range = max(plate_nums) - min(plate_nums)
    
    # Series distribution
    series_counts = defaultdict(int)
    for plate in plates:
        series_counts[get_series_letter(plate)] += 1
    
    # Residuals analysis
    predicted = slope * date_ordinals_sorted + intercept
    residuals = plate_nums_sorted - predicted
    
    # Prediction confidence (95% interval in days)
    confidence_days = 1.96 * std_error / slope
    
    return {
        'plates': plates_sorted,
        'dates': dates_sorted,
        'plate_nums': plate_nums_sorted,
        'date_ordinals': date_ordinals_sorted,
        'slope': slope,
        'intercept': intercept,
        'r_squared': r_squared,
        'std_error': std_error,
        'plates_per_day': plates_per_day,
        'date_range': date_range,
        'plate_range': plate_range,
        'series_counts': dict(series_counts),
        'residuals': residuals,
        'confidence_days': confidence_days,
        'min_date': min(dates),
        'max_date': max(dates),
        'min_plate': min(plates, key=plate_to_num),
        'max_plate': max(plates, key=plate_to_num),
    }


def estimate_observation_lag(data: list, slope: float, intercept: float) -> dict:
    """
    Estimate the lag between actual plate issuance and observation.
    
    Method: For each observed plate, calculate when it "should" have been issued
    based on the regression model, then compare to when it was actually observed.
    The median difference gives us an estimate of observation lag.
    """
    lags = []
    
    for plate, date_str in data:
        obs_date = datetime.strptime(date_str, '%m/%d/%y')
        plate_num = plate_to_num(plate)
        
        # When should this plate have been issued according to model?
        expected_ordinal = (plate_num - intercept) / slope
        expected_date = datetime.fromordinal(int(expected_ordinal))
        
        # Lag = observation date - expected issuance date
        lag_days = (obs_date - expected_date).days
        lags.append(lag_days)
    
    lags = np.array(lags)
    
    return {
        'median_lag': np.median(lags),
        'mean_lag': np.mean(lags),
        'std_lag': np.std(lags),
        'min_lag': np.min(lags),
        'max_lag': np.max(lags),
        'p25_lag': np.percentile(lags, 25),
        'p75_lag': np.percentile(lags, 75),
    }


def analyze_by_time_windows(data: list) -> dict:
    """
    Analyze data using different time windows to see how predictions
    vary based on recent vs historical data.
    
    Windows: 5 years, 3 years, 1 year, 6 months, 1 month
    """
    # Parse all data first
    all_data = []
    for plate, date_str in data:
        date = datetime.strptime(date_str, '%m/%d/%y')
        all_data.append((plate, date, plate_to_num(plate)))
    
    # Sort by date
    all_data.sort(key=lambda x: x[1])
    
    today = datetime.now()
    max_data_date = max(d[1] for d in all_data)
    
    # Define time windows (from most recent data point, not today)
    windows = [
        ('5 years', timedelta(days=5*365)),
        ('3 years', timedelta(days=3*365)),
        ('1 year', timedelta(days=365)),
        ('6 months', timedelta(days=182)),
        ('3 months', timedelta(days=91)),
        ('1 month', timedelta(days=30)),
    ]
    
    target_num = plate_to_num('9ZZZ999')
    results = {}
    
    for window_name, window_delta in windows:
        cutoff_date = max_data_date - window_delta
        
        # Filter data within window
        window_data = [(p, d, n) for p, d, n in all_data if d >= cutoff_date]
        
        if len(window_data) < 3:
            # Not enough data points for regression
            results[window_name] = {
                'n_points': len(window_data),
                'valid': False,
                'reason': 'Insufficient data'
            }
            continue
        
        # Extract arrays
        dates = np.array([d.toordinal() for _, d, _ in window_data])
        plate_nums = np.array([n for _, _, n in window_data])
        
        # Run regression
        slope, intercept, r_squared, std_error = linear_regression(dates, plate_nums)
        
        # Predict final date
        if slope > 0:
            final_ordinal = (target_num - intercept) / slope
            final_date = datetime.fromordinal(int(final_ordinal))
            days_away = (final_date - today).days
            confidence = 1.96 * std_error / slope
        else:
            final_date = None
            days_away = None
            confidence = None
        
        results[window_name] = {
            'n_points': len(window_data),
            'valid': True,
            'slope': slope,
            'intercept': intercept,
            'r_squared': r_squared,
            'plates_per_day': slope,
            'final_date': final_date,
            'days_away': days_away,
            'confidence_days': confidence,
            'date_range': (min(d for _, d, _ in window_data), max(d for _, d, _ in window_data)),
        }
    
    return results


# =============================================================================
# VISUALIZATION - IMPROVED VERSION
# =============================================================================

def create_visualizations(analysis: dict, milestones: dict, window_results: dict = None):
    """Create comprehensive visualization dashboard with no overlapping text."""
    
    # Style setup - larger figure for more space
    plt.style.use('dark_background')
    fig = plt.figure(figsize=(24, 32))  # Even larger for better spacing
    fig.patch.set_facecolor('#0d1117')
    
    # Color palette - NO GREY, only pure colors
    colors = {
        'primary': '#58a6ff',      # Bright blue
        'secondary': '#f78166',    # Coral/orange
        'accent': '#7ee787',       # Bright green
        'warning': '#d29922',      # Gold
        'text': '#ffffff',         # Pure white - NEVER grey
        'grid': '#21262d',         # Subtle dark for grid only
        'alert': '#ff6b6b',        # Red for alerts
        'purple': '#a371f7',       # Purple accent
    }
    
    # Create grid with explicit spacing - 8 rows with better proportions
    gs = GridSpec(8, 4, figure=fig,
                  hspace=0.45, wspace=0.35,
                  left=0.05, right=0.95, top=0.97, bottom=0.02,
                  height_ratios=[0.06, 0.14, 1.1, 0.85, 0.65, 0.75, 0.55, 0.06])
    
    # =========================================================================
    # ROW 0: MAIN TITLE
    # =========================================================================
    ax_title = fig.add_subplot(gs[0, :])
    ax_title.set_facecolor('#0d1117')
    ax_title.axis('off')
    ax_title.text(0.5, 0.5, 'California License Plate 9-Series Runout Analysis',
                  ha='center', va='center', fontsize=26, fontweight='bold',
                  color=colors['text'], transform=ax_title.transAxes)
    
    # =========================================================================
    # ROW 1: DISCLAIMER BOX - Properly spaced
    # =========================================================================
    ax_disclaimer = fig.add_subplot(gs[1, :])
    ax_disclaimer.set_facecolor('#1a1a2e')
    ax_disclaimer.axis('off')
    
    # Single text block with proper line spacing
    disclaimer_text = (
        "IMPORTANT: This analysis is based on OBSERVED license plate sightings, NOT official California DMV issuance records.\n"
        "Observation dates reflect when plates were spotted and reported, which may lag actual issuance by days to weeks.\n"
        "Predictions assume a linear issuance rate and are estimates only.\n\n"
        "WHAT HAPPENS AFTER 9ZZZ999?\n"
        "California will switch to a new format: 000AAA0 (3 numbers, 3 letters, 1 number).\n"
        "Source: LA Times, April 2025 - DMV confirms transition expected in 2026."
    )
    
    ax_disclaimer.text(0.5, 0.5, disclaimer_text, ha='center', va='center', fontsize=11,
                      color=colors['text'], fontweight='normal', transform=ax_disclaimer.transAxes,
                      linespacing=1.5, family='sans-serif',
                      bbox=dict(boxstyle='round,pad=0.8', facecolor='#1a1a2e', 
                               edgecolor=colors['warning'], linewidth=2))
    
    # =========================================================================
    # ROW 2: MAIN TRAJECTORY PLOT (spans 3 columns) + ESTIMATES SIDEBAR
    # =========================================================================
    ax1 = fig.add_subplot(gs[2, 0:3])
    ax1.set_facecolor('#0d1117')
    
    dates_plot = [datetime.fromordinal(int(d)) for d in analysis['date_ordinals']]
    
    # Scatter plot of observed data
    ax1.scatter(dates_plot, analysis['plate_nums'], 
                c=colors['primary'], s=50, alpha=0.9, edgecolors='white', 
                linewidth=0.5, label='Observed plates', zorder=3)
    
    # Regression line extended to milestones
    future_date = milestones['FINAL']['date'] + timedelta(days=30)
    past_date = analysis['min_date'] - timedelta(days=30)
    
    date_range_extended = np.array([past_date.toordinal(), future_date.toordinal()])
    plate_range_extended = analysis['slope'] * date_range_extended + analysis['intercept']
    
    dates_extended = [datetime.fromordinal(int(d)) for d in date_range_extended]
    ax1.plot(dates_extended, plate_range_extended, 
             color=colors['secondary'], linewidth=2.5, linestyle='--', 
             alpha=0.9, label='Trend line', zorder=2)
    
    # ===== DATA GAP ANNOTATION (Oct-Nov 2025) =====
    # Find the gap in our data
    gap_start = datetime(2025, 10, 29)  # Last data point before gap
    gap_end = datetime(2025, 12, 1)     # First data point after gap
    
    # Draw shaded region for the gap
    ax1.axvspan(gap_start, gap_end, alpha=0.25, color=colors['alert'], zorder=1,
                label='Data gap (Oct-Nov 2025)')
    
    # Add annotation explaining the gap - positioned above the trend line
    gap_mid = gap_start + (gap_end - gap_start) / 2
    gap_y = analysis['slope'] * gap_mid.toordinal() + analysis['intercept']
    
    # Position annotation box above the data, in a clear area
    ax1.annotate(
        'DATA GAP (Oct 29 - Dec 1)\nPossible causes: Holiday prep,\nReduced DMV ops, Reporting delays',
        xy=(gap_mid, gap_y),  # Arrow points to middle of gap on trend line
        xytext=(datetime(2025, 5, 1), milestones['9Y']['start_num'] - 100000),  # Text box in clear area
        fontsize=9, color=colors['text'], ha='center', va='center',
        bbox=dict(boxstyle='round,pad=0.4', facecolor='#2d1f1f', edgecolor=colors['alert'], linewidth=2),
        arrowprops=dict(arrowstyle='->', color=colors['alert'], lw=1.5, connectionstyle='arc3,rad=0.3'),
        zorder=10
    )
    
    # Mark key milestones
    milestone_markers = ['9X', '9Y', '9Z', 'FINAL']
    marker_colors = [colors['warning'], colors['accent'], colors['secondary'], colors['alert']]
    
    for ms_key, ms_color in zip(milestone_markers, marker_colors):
        if ms_key == 'FINAL':
            ms = milestones[ms_key]
            ax1.axhline(y=ms['num'], color=ms_color, linestyle=':', alpha=0.7, linewidth=2)
            ax1.scatter([ms['date']], [ms['num']], c=ms_color, s=200, marker='*', 
                       zorder=5, label=f"9ZZZ999: {ms['date'].strftime('%b %Y')}")
        else:
            ms = milestones[ms_key]
            ax1.axhline(y=ms['start_num'], color=ms_color, linestyle=':', alpha=0.5, linewidth=1.5)
            ax1.scatter([ms['start_date']], [ms['start_num']], c=ms_color, s=100, marker='D', 
                       zorder=4, label=f"{ms_key} starts: {ms['start_date'].strftime('%b %Y')}")
    
    ax1.set_xlabel('Date', fontsize=13, color=colors['text'], fontweight='bold')
    ax1.set_ylabel('Plate Sequence Number', fontsize=13, color=colors['text'], fontweight='bold')
    ax1.set_title('License Plate Issuance Trajectory → Predicting End of 9-Series', 
                  fontsize=16, fontweight='bold', color=colors['text'], pad=15)
    ax1.legend(loc='lower right', fontsize=10, facecolor='#161b22', 
               edgecolor=colors['text'], labelcolor=colors['text'])
    ax1.grid(True, alpha=0.3, color=colors['grid'])
    ax1.xaxis.set_major_formatter(mdates.DateFormatter('%b\n%Y'))
    ax1.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
    ax1.tick_params(colors=colors['text'], labelsize=11)
    
    # Format y-axis to show round plate series numbers
    plates_per_series = 26 * 26 * 1000
    y_min = min(analysis['plate_nums'])
    y_max = milestones['FINAL']['num']
    
    series_ticks = []
    series_labels = []
    for letter in LETTERS:
        series_start = LETTERS.index(letter) * plates_per_series
        if y_min - plates_per_series < series_start < y_max + plates_per_series:
            series_ticks.append(series_start)
            series_labels.append(f'9{letter}AA000')
    
    ax1.set_yticks(series_ticks)
    ax1.set_yticklabels(series_labels, fontsize=10, color=colors['text'])
    
    # =========================================================================
    # ROW 2 SIDEBAR: Multi-window estimate comparison
    # =========================================================================
    ax_sidebar = fig.add_subplot(gs[2, 3])
    ax_sidebar.set_facecolor('#0d1117')
    
    if window_results:
        window_names = []
        final_dates = []
        confidences = []
        valid_colors = []
        
        window_color_map = {
            '5 years': '#636efa',
            '3 years': '#00cc96', 
            '1 year': '#ffa15a',
            '6 months': '#ef553b',
            '3 months': '#ab63fa',
            '1 month': '#ff6692',
        }
        
        for name, result in window_results.items():
            if result.get('valid') and result.get('final_date'):
                window_names.append(name)
                final_dates.append(result['final_date'])
                confidences.append(result['confidence_days'])
                valid_colors.append(window_color_map.get(name, colors['primary']))
        
        if final_dates:
            y_positions = np.arange(len(window_names))
            
            for i, (date, conf, color, name) in enumerate(zip(final_dates, confidences, valid_colors, window_names)):
                date_num = mdates.date2num(date)
                
                # Draw confidence interval
                ax_sidebar.barh(i, conf * 2, left=date_num - conf, height=0.6,
                               color=color, alpha=0.4, edgecolor='none')
                
                # Draw point estimate
                ax_sidebar.scatter([date_num], [i], c=color, s=120, zorder=5, 
                                  edgecolors='white', linewidth=1.5)
                
                # Label with date - positioned to not overlap
                ax_sidebar.text(date_num, i + 0.4, date.strftime('%b %d'), 
                               ha='center', va='bottom', fontsize=10, 
                               color=colors['text'], fontweight='bold')
            
            ax_sidebar.set_yticks(y_positions)
            ax_sidebar.set_yticklabels(window_names, fontsize=10, color=colors['text'])
            ax_sidebar.set_xlabel('Predicted Date for 9ZZZ999', fontsize=10, 
                                 color=colors['text'], fontweight='bold')
            ax_sidebar.set_title('Estimates by Data Window\n(bars = ±95% confidence)', 
                                fontsize=12, fontweight='bold', color=colors['text'], pad=8)
            ax_sidebar.xaxis.set_major_formatter(mdates.DateFormatter('%b\n%Y'))
            ax_sidebar.xaxis.set_major_locator(mdates.MonthLocator())
            ax_sidebar.tick_params(colors=colors['text'], labelsize=9)
            ax_sidebar.grid(True, axis='x', alpha=0.3, color=colors['grid'])
            # Ensure y-axis labels have enough space
            ax_sidebar.yaxis.set_tick_params(pad=2)
    
    # =========================================================================
    # ROW 3: RESIDUALS PLOT + EXPLANATION + PLATES REMAINING
    # =========================================================================
    
    # Residuals plot (left)
    ax_resid = fig.add_subplot(gs[3, 0:2])
    ax_resid.set_facecolor('#0d1117')
    
    final_date = milestones['FINAL']['date']
    
    ax_resid.scatter(dates_plot, analysis['residuals'], c=colors['accent'], s=35, alpha=0.8)
    ax_resid.axhline(y=0, color=colors['secondary'], linestyle='-', alpha=0.9, linewidth=2)
    ax_resid.fill_between(dates_plot, 
                          -analysis['std_error'], analysis['std_error'],
                          alpha=0.25, color=colors['primary'])
    
    # Mark final date
    ax_resid.axvline(x=final_date, color=colors['alert'], linestyle='--', alpha=0.8, linewidth=2)
    
    ax_resid.set_xlim(analysis['min_date'] - timedelta(days=15), final_date + timedelta(days=15))
    ax_resid.set_xlabel('Date', fontsize=12, color=colors['text'], fontweight='bold')
    ax_resid.set_ylabel('Residual (plates)', fontsize=12, color=colors['text'], fontweight='bold')
    ax_resid.set_title('Model Residuals: How Well Does Our Prediction Fit?', 
                       fontsize=14, fontweight='bold', color=colors['text'], pad=10)
    ax_resid.grid(True, alpha=0.3, color=colors['grid'])
    ax_resid.xaxis.set_major_formatter(mdates.DateFormatter('%b\n%y'))
    ax_resid.xaxis.set_major_locator(mdates.MonthLocator(interval=3))
    ax_resid.tick_params(colors=colors['text'], labelsize=10)
    
    # Residuals explanation box (middle-right)
    ax_resid_explain = fig.add_subplot(gs[3, 2])
    ax_resid_explain.set_facecolor('#161b22')
    ax_resid_explain.axis('off')
    
    residual_explanation = """UNDERSTANDING RESIDUALS

What are residuals?
Residuals = Observed plate# − Model prediction

What do they tell us?
• POSITIVE: Plate observed LATER than predicted
• NEGATIVE: Plate observed EARLIER than predicted
• ZERO line: Perfect model prediction

The shaded band = ±1 standard error
(~68% of observations fall within)

Why this matters:
• Tight clustering = good model fit
• Systematic patterns = missing effects
• Our R² = {r2:.3f} ({quality} fit)

Model predicts ~{ppd:,.0f} plates/day""".format(
        r2=analysis['r_squared'],
        quality='EXCELLENT' if analysis['r_squared'] > 0.95 else 'GOOD' if analysis['r_squared'] > 0.9 else 'FAIR',
        ppd=analysis['plates_per_day']
    )
    
    ax_resid_explain.text(0.05, 0.98, residual_explanation, transform=ax_resid_explain.transAxes,
                          fontsize=9, color=colors['text'], va='top', ha='left',
                          family='monospace', linespacing=1.3,
                          bbox=dict(boxstyle='round,pad=0.4', facecolor='#161b22', 
                                   edgecolor=colors['accent'], linewidth=2))
    
    ax_resid_explain.set_title('Residuals Explained', fontsize=11, fontweight='bold', 
                               color=colors['accent'], pad=3)
    
    # Plates remaining (right)
    ax_remaining = fig.add_subplot(gs[3, 3])
    ax_remaining.set_facecolor('#0d1117')
    
    final_num = milestones['FINAL']['num']
    today = datetime.now()
    current_plate_num = predict_plate(today, analysis['slope'], analysis['intercept'])
    
    remaining_series = []
    remaining_plates = []
    series_colors_list = []
    
    for i, letter in enumerate(['X', 'Y', 'Z']):
        ms = milestones[f'9{letter}']
        if current_plate_num < ms['start_num']:
            remaining = ms['end_num'] - ms['start_num'] + 1
        elif current_plate_num <= ms['end_num']:
            remaining = ms['end_num'] - current_plate_num
        else:
            remaining = 0
        
        if remaining > 0:
            remaining_series.append(f'9{letter}')
            remaining_plates.append(remaining / 1000)
            series_colors_list.append(plt.cm.plasma(0.4 + 0.2 * i))
    
    bars = ax_remaining.barh(remaining_series, remaining_plates, color=series_colors_list, 
                             edgecolor='white', linewidth=1)
    ax_remaining.set_xlabel('Plates (thousands)', fontsize=10, 
                            color=colors['text'], fontweight='bold')
    
    # Title with total count - combined to avoid overlap
    total_remaining = final_num - current_plate_num
    ax_remaining.set_title(f'Plates Until 9ZZZ999\n({total_remaining:,} remaining)', 
                           fontsize=11, fontweight='bold', color=colors['text'], pad=5,
                           linespacing=1.2)
    ax_remaining.tick_params(colors=colors['text'], labelsize=10)
    
    # Adjust x-axis to leave room for labels
    max_plates = max(remaining_plates) if remaining_plates else 700
    ax_remaining.set_xlim(0, max_plates * 1.15)
    
    for bar, count in zip(bars, remaining_plates):
        ax_remaining.text(bar.get_width() + 8, bar.get_y() + bar.get_height()/2, 
                         f'{count:.0f}K', ha='left', va='center', fontsize=10, 
                         color=colors['text'], fontweight='bold')
    
    # =========================================================================
    # ROW 4: TIMELINE
    # =========================================================================
    ax_timeline = fig.add_subplot(gs[4, :])
    ax_timeline.set_facecolor('#0d1117')
    
    timeline_milestones = []
    for letter in ['P', 'Q', 'R', 'S', 'T', 'U', 'V', 'W', 'X', 'Y', 'Z']:
        ms = milestones[f'9{letter}']
        timeline_milestones.append({
            'label': f'9{letter}',
            'start': ms['start_date'],
            'end': ms['end_date'],
        })
    
    current_plate = num_to_plate(max(0, min(current_plate_num, plate_to_num('9ZZZ999'))))
    
    y_pos = 0
    bar_height = 0.7
    
    for i, ms in enumerate(timeline_milestones):
        color_intensity = 0.25 + 0.07 * i
        bar_color = plt.cm.plasma(color_intensity)
        
        start_num = mdates.date2num(ms['start'])
        end_num = mdates.date2num(ms['end'])
        width = end_num - start_num
        
        rect = plt.Rectangle((start_num, y_pos - bar_height/2), width, bar_height,
                             facecolor=bar_color, edgecolor='white', linewidth=0.8, alpha=0.85)
        ax_timeline.add_patch(rect)
        
        mid_date = ms['start'] + (ms['end'] - ms['start']) / 2
        ax_timeline.text(mdates.date2num(mid_date), y_pos, ms['label'], 
                        ha='center', va='center', fontsize=11, fontweight='bold', 
                        color=colors['text'])
        
        y_pos += 1
    
    # Mark holiday period
    holiday_start = datetime(2025, 12, 20)
    holiday_end = datetime(2026, 1, 5)
    ax_timeline.axvspan(mdates.date2num(holiday_start), mdates.date2num(holiday_end),
                        alpha=0.35, color=colors['alert'], zorder=1)
    ax_timeline.text(mdates.date2num(datetime(2025, 12, 28)), len(timeline_milestones) - 0.2, 
                     'Holiday\nPeriod', ha='center', va='top', fontsize=9, 
                     color=colors['text'], fontweight='bold',
                     bbox=dict(boxstyle='round,pad=0.2', facecolor=colors['alert'], alpha=0.7))
    
    # Mark today
    ax_timeline.axvline(x=mdates.date2num(today), color=colors['alert'], linestyle='-', 
                        linewidth=3, alpha=0.95)
    ax_timeline.text(mdates.date2num(today), -1.0, f'TODAY\n{current_plate}', 
                     ha='center', va='top', fontsize=10, color=colors['text'], 
                     fontweight='bold',
                     bbox=dict(boxstyle='round,pad=0.3', facecolor=colors['alert'], alpha=0.8))
    
    # Mark final date
    ax_timeline.axvline(x=mdates.date2num(final_date), color=colors['accent'], linestyle='--', 
                        linewidth=2.5, alpha=0.95)
    ax_timeline.text(mdates.date2num(final_date + timedelta(days=5)), -1.0, 
                     f'9ZZZ999\n{final_date.strftime("%b %d, %Y")}',
                     ha='left', va='top', fontsize=10, color=colors['text'], 
                     fontweight='bold',
                     bbox=dict(boxstyle='round,pad=0.3', facecolor=colors['accent'], alpha=0.8))
    
    ax_timeline.set_xlim(mdates.date2num(datetime(2024, 1, 1)), 
                         mdates.date2num(final_date + timedelta(days=75)))
    ax_timeline.set_ylim(-1.8, len(timeline_milestones) + 0.2)
    ax_timeline.set_xlabel('Date', fontsize=12, color=colors['text'], fontweight='bold')
    ax_timeline.set_title(f'Series Timeline: 9-Series Exhaustion  →  Final: {final_date.strftime("%B %d, %Y")}', 
                          fontsize=14, fontweight='bold', color=colors['text'], pad=12)
    ax_timeline.xaxis.set_major_formatter(mdates.DateFormatter('%b %Y'))
    ax_timeline.xaxis.set_major_locator(mdates.MonthLocator(interval=2))
    ax_timeline.set_yticks([])
    ax_timeline.tick_params(colors=colors['text'], labelsize=10)
    ax_timeline.grid(True, axis='x', alpha=0.3, color=colors['grid'])
    
    # =========================================================================
    # ROW 5: ISSUANCE RATE CHART + EXPLANATION
    # =========================================================================
    
    # Rate chart (left 2 columns)
    ax_rate = fig.add_subplot(gs[5, 0:2])
    ax_rate.set_facecolor('#0d1117')
    
    if window_results:
        valid_windows = [(name, r) for name, r in window_results.items() if r.get('valid')]
        if valid_windows:
            names = [w[0] for w in valid_windows]
            rates_vals = [w[1]['plates_per_day'] for w in valid_windows]
            
            window_colors_list = ['#636efa', '#00cc96', '#ffa15a', '#ef553b', '#ab63fa', '#ff6692']
            bar_colors = window_colors_list[:len(names)]
            
            bars = ax_rate.bar(names, rates_vals, color=bar_colors, edgecolor='white', linewidth=1)
            
            for bar, rate in zip(bars, rates_vals):
                ax_rate.text(bar.get_x() + bar.get_width()/2, bar.get_height() + 200, 
                            f'{rate:,.0f}', ha='center', va='bottom', fontsize=11, 
                            color=colors['text'], fontweight='bold')
            
            avg_rate = np.mean(rates_vals)
            ax_rate.axhline(y=avg_rate, color=colors['accent'], linestyle='--', linewidth=2, alpha=0.9)
            ax_rate.text(len(names) - 0.5, avg_rate + 200, f'Avg: {avg_rate:,.0f}', 
                        ha='right', va='bottom', fontsize=11, color=colors['accent'], fontweight='bold')
            
            ax_rate.set_xlabel('Data Window', fontsize=12, color=colors['text'], fontweight='bold')
            ax_rate.set_ylabel('Plates per Day', fontsize=12, color=colors['text'], fontweight='bold')
            ax_rate.set_title('Issuance Rate by Data Window', 
                             fontsize=14, fontweight='bold', color=colors['text'], pad=10)
            ax_rate.tick_params(colors=colors['text'], labelsize=11)
            ax_rate.grid(True, axis='y', alpha=0.3, color=colors['grid'])
    
    # Rate explanation (right 2 columns)
    ax_rate_explain = fig.add_subplot(gs[5, 2:4])
    ax_rate_explain.set_facecolor('#161b22')
    ax_rate_explain.axis('off')
    
    rate_explanation = """UNDERSTANDING DATA WINDOWS

What is a "data window"?
Each bar = issuance rate from that time period:
• "1 year" = last 12 months of observations
• "1 month" = last 30 days only

Why do rates differ?
• SEASONAL: Sales peak in spring/summer
• HOLIDAYS: Dec-Jan reduced DMV activity
• ECONOMIC: Interest rates, gas prices
• LAG: Observers delay reporting

Which window to trust?
• 5-YEAR: Most data, may be outdated
• 1-YEAR: Best balance (RECOMMENDED)
• 3-MONTH: Skewed by holidays
• 1-MONTH: Too volatile

INTERPRETATION:
Higher recent rates → ACCELERATING
Lower recent rates → DECELERATING"""
    
    ax_rate_explain.text(0.05, 0.98, rate_explanation, transform=ax_rate_explain.transAxes,
                         fontsize=9, color=colors['text'], va='top', ha='left',
                         family='monospace', linespacing=1.25,
                         bbox=dict(boxstyle='round,pad=0.4', facecolor='#161b22', 
                                  edgecolor=colors['primary'], linewidth=2))
    
    ax_rate_explain.set_title('Data Windows Explained', fontsize=11, fontweight='bold', 
                              color=colors['primary'], pad=3)
    
    # =========================================================================
    # ROW 6: METHODOLOGY SUMMARY - Compact version
    # =========================================================================
    ax_method = fig.add_subplot(gs[6, :])
    ax_method.set_facecolor('#161b22')
    ax_method.axis('off')
    
    method_text = """METHODOLOGY SUMMARY

DATA SOURCE: Crowdsourced license plate sightings from California. Each point = when a plate was observed "in the wild."
             NOT official DMV data - inherent lag between issuance and first sighting.

MODEL: Linear regression (plate# vs. observation date). Assumes roughly constant issuance rate.
       R² = {r2:.4f} → model explains {r2pct:.1f}% of variance.

PREDICTION: Extrapolating to plate #17,575,999 (9ZZZ999) gives estimated exhaustion date.
            95% confidence: ±{conf:.0f} days.

LIMITATIONS: Observation lag, seasonal variations, DMV policy changes, economic factors affect actual rates. This is an estimate.""".format(
        r2=analysis['r_squared'],
        r2pct=analysis['r_squared'] * 100,
        conf=analysis['confidence_days']
    )
    
    ax_method.text(0.5, 0.5, method_text, transform=ax_method.transAxes,
                   fontsize=9, color=colors['text'], va='center', ha='center',
                   family='monospace', linespacing=1.5,
                   bbox=dict(boxstyle='round,pad=0.6', facecolor='#161b22', 
                            edgecolor=colors['purple'], linewidth=2))
    
    # =========================================================================
    # ROW 7: FOOTER
    # =========================================================================
    ax_footer = fig.add_subplot(gs[7, :])
    ax_footer.set_facecolor('#0d1117')
    ax_footer.axis('off')
    
    footer_text = f"Generated: {datetime.now().strftime('%Y-%m-%d %H:%M')}  |  Data points: {len(analysis['plates'])}  |  Date range: {analysis['min_date'].strftime('%b %Y')} - {analysis['max_date'].strftime('%b %Y')}"
    ax_footer.text(0.5, 0.5, footer_text, ha='center', va='center', fontsize=10,
                   color=colors['text'], transform=ax_footer.transAxes)
    
    # Save with unique filename
    output_file = get_unique_filename('plate_analysis', '.png')
    plt.savefig(output_file, dpi=120, facecolor='#0d1117', edgecolor='none',
                bbox_inches='tight', pad_inches=0.3)
    print(f"\nChart saved to: {output_file}")
    plt.close()
    
    return output_file


def print_report(analysis: dict, milestones: dict, lag_stats: dict = None):
    """Print detailed analysis report."""
    
    print("\n" + "="*70)
    print("  CALIFORNIA LICENSE PLATE 9-SERIES RUNOUT ANALYSIS")
    print("="*70)
    print("  NOTE: Based on OBSERVED plate sightings, not official DMV issuance.")
    print("  Observation dates may lag actual issuance. Estimates assume linear rate.")
    print("="*70)
    
    print("\n[DATA SUMMARY]")
    print("-"*40)
    print(f"  Data points:        {len(analysis['plates'])}")
    print(f"  Date range:         {analysis['min_date'].strftime('%b %d, %Y')} -> {analysis['max_date'].strftime('%b %d, %Y')}")
    print(f"  Plate range:        {analysis['min_plate']} -> {analysis['max_plate']}")
    print(f"  Time span:          {analysis['date_range'].days} days")
    
    print("\n[REGRESSION MODEL]")
    print("-"*40)
    quality = 'Excellent' if analysis['r_squared'] > 0.95 else 'Good' if analysis['r_squared'] > 0.9 else 'Fair'
    print(f"  R-squared (fit):    {analysis['r_squared']:.4f}  ({quality})")
    print(f"  Plates per day:     {analysis['plates_per_day']:,.0f}")
    print(f"  Plates per month:   {analysis['plates_per_day'] * 30.44:,.0f}")
    print(f"  Plates per year:    {analysis['plates_per_day'] * 365.25:,.0f}")
    print(f"  Prediction +/-95%:  +/-{analysis['confidence_days']:.0f} days")
    
    if lag_stats:
        print("\n[OBSERVATION LAG ANALYSIS]")
        print("-"*40)
        print("  (Estimated delay between issuance and observation)")
        print(f"  Median lag:         {lag_stats['median_lag']:.0f} days")
        print(f"  Mean lag:           {lag_stats['mean_lag']:.1f} days")
        print(f"  Std deviation:      {lag_stats['std_lag']:.1f} days")
        print(f"  Range:              {lag_stats['min_lag']:.0f} to {lag_stats['max_lag']:.0f} days")
        print(f"  25th-75th pctile:   {lag_stats['p25_lag']:.0f} to {lag_stats['p75_lag']:.0f} days")
        print("\n  Interpretation:")
        if lag_stats['median_lag'] > 0:
            print(f"    Observations lag issuance by ~{lag_stats['median_lag']:.0f} days on average.")
            print(f"    Actual release dates are likely ~{abs(lag_stats['median_lag']):.0f} days EARLIER than predicted.")
        else:
            print(f"    Model suggests plates observed ~{abs(lag_stats['median_lag']):.0f} days before expected.")
    
    print("\n[MILESTONE PREDICTIONS]")
    print("-"*40)
    
    today = datetime.now()
    
    for letter in ['X', 'Y', 'Z']:
        ms = milestones[f'9{letter}']
        days_to_start = (ms['start_date'] - today).days
        days_to_end = (ms['end_date'] - today).days
        duration = (ms['end_date'] - ms['start_date']).days
        
        status = ""
        if days_to_start > 0:
            status = f"starts in {days_to_start} days"
        elif days_to_end > 0:
            status = "IN PROGRESS"
        else:
            status = "COMPLETE"
        
        print(f"\n  >> 9{letter} SERIES ({status})")
        print(f"     Start: {ms['start_plate']} on {ms['start_date'].strftime('%b %d, %Y')}")
        print(f"     End:   {ms['end_plate']} on {ms['end_date'].strftime('%b %d, %Y')}")
        print(f"     Duration: {duration} days ({duration/30.44:.1f} months)")
    
    print("\n  " + "-"*36)
    final = milestones['FINAL']
    days_to_final = (final['date'] - today).days
    print(f"\n  *** FINAL PLATE: {final['plate']} ***")
    print(f"     Predicted:   {final['date'].strftime('%B %d, %Y')}")
    print(f"     Days away:   {days_to_final:,} ({days_to_final/365.25:.1f} years)")
    
    # Current plate estimate
    current_num = predict_plate(today, analysis['slope'], analysis['intercept'])
    if 0 <= current_num <= plate_to_num('9ZZZ999'):
        current_plate = num_to_plate(current_num)
        print(f"\n  [>] Current estimate: ~{current_plate}")
    
    print("\n" + "="*70)


def print_window_analysis(window_results: dict):
    """Print multi-window analysis comparison."""
    
    print("\n" + "="*70)
    print("  MULTI-WINDOW ESTIMATE COMPARISON")
    print("  (How predictions vary based on data recency)")
    print("="*70)
    
    print("\n  Window       | Points |  R^2   | Plates/Day | 9ZZZ999 Predicted  | +/- Days")
    print("  " + "-"*78)
    
    for window_name, result in window_results.items():
        if not result['valid']:
            print(f"  {window_name:12} | {result['n_points']:>6} | {'--':>6} | {'--':>10} | {result['reason']:<18} |")
        else:
            date_str = result['final_date'].strftime('%b %d, %Y') if result['final_date'] else 'N/A'
            conf = f"+/-{result['confidence_days']:.0f}" if result['confidence_days'] else 'N/A'
            print(f"  {window_name:12} | {result['n_points']:>6} | {result['r_squared']:.4f} | {result['plates_per_day']:>10,.0f} | {date_str:<18} | {conf}")
    
    print("  " + "-"*78)
    
    # Calculate spread of predictions
    valid_dates = [r['final_date'] for r in window_results.values() if r.get('valid') and r.get('final_date')]
    if len(valid_dates) >= 2:
        earliest = min(valid_dates)
        latest = max(valid_dates)
        spread = (latest - earliest).days
        print(f"\n  Prediction spread: {spread} days ({earliest.strftime('%b %d')} to {latest.strftime('%b %d, %Y')})")
        
        # Show trend in issuance rate
        rates = [(name, r['plates_per_day']) for name, r in window_results.items() if r.get('valid')]
        if len(rates) >= 2:
            recent_rate = rates[-1][1]  # Most recent window
            oldest_rate = rates[0][1]   # Oldest window
            if oldest_rate > 0:
                change_pct = ((recent_rate - oldest_rate) / oldest_rate) * 100
                trend = "ACCELERATING" if change_pct > 5 else "DECELERATING" if change_pct < -5 else "STABLE"
                print(f"  Issuance rate trend: {trend} ({change_pct:+.1f}% from oldest to most recent window)")
    
    print("="*70)


# =============================================================================
# MAIN
# =============================================================================

if __name__ == '__main__':
    # Run analysis
    analysis = analyze_data(data)
    milestones = calculate_milestones(analysis['slope'], analysis['intercept'])
    
    # Multi-window analysis
    window_results = analyze_by_time_windows(data)
    
    # Estimate observation lag
    lag_stats = estimate_observation_lag(data, analysis['slope'], analysis['intercept'])
    
    # Print reports
    print_report(analysis, milestones, lag_stats)
    print_window_analysis(window_results)
    
    # Generate visualizations
    output_file = create_visualizations(analysis, milestones, window_results)
    print(f"\nVisualization complete: {output_file}")
