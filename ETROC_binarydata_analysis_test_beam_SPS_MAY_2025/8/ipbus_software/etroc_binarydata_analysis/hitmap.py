import os
import glob
import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns

plt.rcParams.update({
    'font.size': 12,
    'axes.titlesize': 14,
    'axes.labelsize': 12,
    'xtick.labelsize': 10,
    'ytick.labelsize': 10
})

def get_stat(df, key, default=0):
    return df.loc[key, "Value"] if key in df.index else default

def make_pie(ax, data, labels, colors, title, ncol=1, extra_text=None):
    """지정된 스타일(이미지 레퍼런스)로 파이 차트와 범례를 생성하는 헬퍼 함수"""
    valid = [(v, l, c) for v, l, c in zip(data, labels, colors) if v > 0]
    if not valid:
        ax.axis('off')
        ax.set_title(title + "\n(No Data)", fontweight='bold')
        return
    
    vals = [item[0] for item in valid]
    lbls = [item[1] for item in valid]
    cols = [item[2] for item in valid]
    total = sum(vals)

    wedges, texts, autotexts = ax.pie(
        vals, 
        autopct=lambda p: f'{p:.2f}%' if p > 0 else '',
        startangle=90, 
        colors=cols, 
        textprops={'fontsize': 11}
    )
    ax.set_title(title, fontweight='bold', fontsize=14)

    # 범례 텍스트 생성 (개수 및 퍼센트 포함)
    legend_labels = [f"{l}: {int(v):,} ({v/total*100:.1f}%)" for v, l in zip(vals, lbls)]

    # 추가 텍스트(예: Avg Jump Size)가 있을 경우 범례 위치를 더 아래로 조정
    bbox_y = -0.18 if extra_text else -0.1
    if extra_text:
        ax.text(0.5, -0.05, extra_text, transform=ax.transAxes, color='red', 
                fontweight='bold', ha='center', fontsize=12)

    ax.legend(wedges, legend_labels, loc="upper center", bbox_to_anchor=(0.5, bbox_y), 
              ncol=ncol, fontsize=11, framealpha=0.9)


def visualize_pair(hm_file, st_file):
    base = os.path.basename(hm_file).replace("_heatmap.csv", "")

    if not os.path.exists(hm_file) or not os.path.exists(st_file):
        print(f"Skipping: {hm_file} or {st_file} not found.")
        return

    df_stats = pd.read_csv(st_file, index_col="Metric")

    # 3x3 그리드 생성
    fig, axes = plt.subplots(3, 3, figsize=(24, 26))
    fig.suptitle(f"ETROC2 Analysis Dashboard: RIGHT Channel\nDataset: {base}", 
                 fontsize=26, fontweight='bold', y=0.96)

    # ==========================================
    # [0, 0] Data Map (Raw) & System Status Text
    # ==========================================
    df_hm = pd.read_csv(hm_file, header=None)
    sns.heatmap(df_hm, ax=axes[0, 0], annot=False, cmap="viridis", linewidths=1.0, square=True)
    axes[0, 0].invert_yaxis()
    axes[0, 0].invert_xaxis()
    axes[0, 0].set_title("Data Map (Raw)", fontweight='bold')

    bw = get_stat(df_stats, "Link_Bandwidth_Mbps")
    hw_drops = get_stat(df_stats, "Total_HW_Drops")
    miss_trl = get_stat(df_stats, "Events_Missing_Trailer", 0)
    miss_hdr = get_stat(df_stats, "Events_Missing_Header", 0)
    orphan = get_stat(df_stats, "Orphan_Data_Words", 0)

    sys_text = (f"[ System Status ]\n"
                f" ├ Bandwidth: {bw:.2f} Mbps\n"
                f" └ HW Drops: {int(hw_drops)} (Clean)\n"
                f"────────────────\n"
                f"[ Excluded (Incomplete) ]\n"
                f" ├ Missing Trailer: {int(miss_trl)}\n"
                f" ├ Missing Header: {int(miss_hdr)}\n"
                f" └ Orphan Data: {int(orphan)}")
    
    axes[0, 0].text(0.5, -0.15, sys_text, transform=axes[0, 0].transAxes, fontsize=12,
                    verticalalignment='top', horizontalalignment='left',
                    bbox=dict(boxstyle='round,pad=0.5', facecolor='#f8f9fa', edgecolor='silver', alpha=0.9))

    # ==========================================
    # [0, 1] Frame Composition
    # ==========================================
    frames_total = get_stat(df_stats, "Total_Frames")
    frames_data = [get_stat(df_stats, "Raw_Hit_Frames"), get_stat(df_stats, "Headers"), 
                   get_stat(df_stats, "Trailers"), get_stat(df_stats, "Fillers")]
    frames_lbls = ["Data Words", "Headers", "Trailers", "Fillers"]
    frames_cols = ['#9b59b6', '#3498db', '#e67e22', '#bdc3c7'] # 보라, 파랑, 주황, 회색
    
    make_pie(axes[0, 1], frames_data, frames_lbls, frames_cols, 
             f"Frame Composition\n(Total: {int(frames_total):,})", ncol=2)

    # ==========================================
    # [0, 2] L1 Counter Continuity
    # ==========================================
    total_headers = get_stat(df_stats, "Headers")
    l1_jumps = get_stat(df_stats, "L1_Counter_Jumps")
    l1_continuous = total_headers - l1_jumps
    avg_jump = get_stat(df_stats, "L1_Jump_Avg")

    make_pie(axes[0, 2], [l1_continuous, l1_jumps], ["Continuous (Normal)", "L1 Jumps (Discarded)"], 
             ['#4CAF50', '#F44336'], f"L1 Counter Continuity\n(Total Headers: {int(total_headers):,})", 
             extra_text=f"Avg Jump Size: {avg_jump:.5f}")

    # ==========================================
    # [1, 0] L1 Buffer Status
    # ==========================================
    buf_data = [get_stat(df_stats, "Buffer_Normal"), get_stat(df_stats, "Buffer_HalfFull"),
                get_stat(df_stats, "Buffer_Overflow"), get_stat(df_stats, "Buffer_Full")]
    buf_lbls = ['Normal', 'Half Full', 'Overflow', 'Full']
    buf_cols = ['#66c2a5', '#fc8d62', '#8da0cb', '#e78ac3']

    make_pie(axes[1, 0], buf_data, buf_lbls, buf_cols, "L1 Buffer Status\n(Parsed from Trailer)")

    # ==========================================
    # [1, 1] EA (Error Flag) Ratio
    # ==========================================
    hit_data = get_stat(df_stats, "Valid_Hits", get_stat(df_stats, "Raw_Hit_Frames"))
    ea_data = [get_stat(df_stats, "EA_00_Clean"), get_stat(df_stats, "EA_01_Corrected"),
               get_stat(df_stats, "EA_10_Uncorrectable"), get_stat(df_stats, "EA_11_Undefined")]
    ea_lbls = ['00 (Clean)', '01 (Corrected)', '10 (Fatal)', '11 (Undef.)']
    ea_cols = ['#abcdef', '#ffcc99', '#ff9999', '#cccccc']

    make_pie(axes[1, 1], ea_data, ea_lbls, ea_cols, f"EA (Error Flag) Ratio\n(Total Data: {int(hit_data):,})")

    # ==========================================
    # [1, 2] Strict Event CRC Check
    # ==========================================
    crc_pass = get_stat(df_stats, "CRC_Match")
    crc_fail = get_stat(df_stats, "CRC_Mismatch")
    crc_total = crc_pass + crc_fail

    make_pie(axes[1, 2], [crc_pass, crc_fail], ["Pass", "Fail"], 
             ['#2196F3', '#FF9800'], f"Strict Event CRC Check\n(Checked Events: {int(crc_total):,})")

    # ==========================================
    # [2, 0] CRC Mismatch Causes (Bar Chart)
    # ==========================================
    ax_bar = axes[2, 0]
    causes_val = [get_stat(df_stats, "CRC_Err_MissingData"), get_stat(df_stats, "CRC_Err_EA_Fatal"),
                  get_stat(df_stats, "CRC_Err_MissingData_With_EA"), get_stat(df_stats, "CRC_Err_SilentFlip")]
    causes_lbl = ["Missing\nHit", "EA Fatal\nError", "Both\n(Overlap)", "Silent\nFlip"]
    bar_cols = ['#ff9999', '#c2c2f0', '#99e699', '#ffcc99']

    bars = ax_bar.bar(causes_lbl, causes_val, color=bar_cols, edgecolor='gray', width=0.6)
    ax_bar.set_title("CRC Mismatch Causes\n(Overlapping Allowed)", fontweight='bold')

    # 총 CRC 실패 점선 표시
    ax_bar.axhline(y=crc_fail, color='red', linestyle='--', label=f"Total CRC Fails ({int(crc_fail)})")
    ax_bar.legend(loc='upper left', fontsize=11)

    # 바 위에 수치 표시
    for bar in bars:
        yval = bar.get_height()
        if yval > 0:
            ax_bar.text(bar.get_x() + bar.get_width()/2, yval + (crc_fail * 0.02), 
                        int(yval), ha='center', va='bottom', fontweight='bold', fontsize=12)
    
    ax_bar.set_ylim(0, crc_fail * 1.15 if crc_fail > 0 else 10)

    # ==========================================
    # [2, 1] Data vs Header BCID
    # ==========================================
    bcid_matched = get_stat(df_stats, "BCID_Match")
    bcid_mismatch = get_stat(df_stats, "BCID_Mismatch_Only")

    make_pie(axes[2, 1], [bcid_matched, bcid_mismatch], ["Matched", "Pure BCID Mismatch"], 
             ['#4CAF50', '#F44336'], f"Data vs Header BCID\n(Checked: {int(bcid_matched + bcid_mismatch):,})")

    # ==========================================
    # [2, 2] Event Structural Integrity
    # ==========================================
    perfect = get_stat(df_stats, "Events_Perfect_Hits")
    hit_mismatch = get_stat(df_stats, "Events_Hit_Mismatch")
    corr_bcid = get_stat(df_stats, "Events_Corrupted_BCID")
    total_struct = perfect + hit_mismatch + corr_bcid

    make_pie(axes[2, 2], [perfect, hit_mismatch, corr_bcid], 
             ["Perfect (CRC Checked)", "Hit Mismatch (CRC Checked)", "Corrupted BCID (Skipped)"],
             ['#8BC34A', '#FFB300', '#E53935'], f"Event Structural Integrity\n(Target Events: {int(total_struct):,})")

    # 간격 조정 및 저장 (범례가 잘리지 않도록 bbox_inches 적용)
    plt.subplots_adjust(hspace=0.5, wspace=0.3, bottom=0.1)
    png_filename = f"{base}_dashboard.png"
    plt.savefig(png_filename, dpi=150, bbox_inches='tight')
    print(f"Dashboard Saved: {png_filename}")
    plt.close()


if __name__ == "__main__":
    heatmap_files = sorted(glob.glob("batch*_heatmap.csv"))

    if not heatmap_files:
        print("No batch*_heatmap.csv files found in current folder.")
    else:
        for hm_file in heatmap_files:
            st_file = hm_file.replace("_heatmap.csv", "_stats.csv")
            visualize_pair(hm_file, st_file)