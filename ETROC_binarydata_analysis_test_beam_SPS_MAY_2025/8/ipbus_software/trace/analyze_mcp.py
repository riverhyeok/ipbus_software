import numpy as np
import lecroyparser
import matplotlib.pyplot as plt
import csv

ch3_file = "raw_data/C3--Trace119192.trc"
NUM_SEGMENTS = 5000
WINDOW_NS = 25.0

try:
    print(f"Reading MCP TRC file: {ch3_file} ...")
    data_ch3 = lecroyparser.ScopeData(ch3_file)
    y_raw = np.array(data_ch3.y)
    
    total_points = len(y_raw)
    points_per_seg = total_points // NUM_SEGMENTS
    
    y_valid = y_raw[:points_per_seg * NUM_SEGMENTS]
    y_segments = y_valid.reshape((NUM_SEGMENTS, points_per_seg))
    time_axis_ns = np.linspace(0, WINDOW_NS, points_per_seg)

    mcp_peak_times = []
    mcp_peak_volts = []
    
    for i in range(NUM_SEGMENTS):
        seg_data = y_segments[i]
        min_idx = np.argmin(seg_data) # Negative signal
        min_volt = seg_data[min_idx]
        peak_time = time_axis_ns[min_idx]
        
        mcp_peak_times.append(peak_time)
        mcp_peak_volts.append(min_volt)

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6))
    
    plot_count = min(100, NUM_SEGMENTS)
    for i in range(plot_count):
        ax1.plot(time_axis_ns, y_segments[i], color='purple', alpha=0.1)
    ax1.plot(time_axis_ns, np.mean(y_segments, axis=0), color='red', linewidth=2, label='Mean Waveform')
    ax1.set_title(f'MCP Waveforms (Ch3) - {plot_count} Overlaid')
    ax1.set_xlabel('Time (ns)')
    ax1.set_ylabel('Voltage (V)')
    ax1.grid(True, alpha=0.5)
    ax1.legend()

    ax2.hist(mcp_peak_times, bins=100, color='teal', alpha=0.7)
    ax2.set_title('Extracted MCP Peak Time Distribution')
    ax2.set_xlabel('Peak Time (ns)')
    ax2.set_ylabel('Events count')
    ax2.grid(True, alpha=0.5)

    plt.tight_layout()
    output_img = "results/mcp_analysis_plot.png"
    plt.savefig(output_img, dpi=300)
    
    csv_file = "results/mcp_timing_reference.csv"
    with open(csv_file, mode='w', newline='') as file:
        writer = csv.writer(file)
        writer.writerow(["Event_ID", "MCP_Peak_Time_ns", "MCP_Peak_Volt"])
        for i in range(NUM_SEGMENTS):
            writer.writerow([i, mcp_peak_times[i], mcp_peak_volts[i]])

    print(f"동기화용 절대 시간 데이터가 '{csv_file}'로 저장되었습니다.")

except Exception as e:
    print(f"오류가 발생했습니다: {e}")