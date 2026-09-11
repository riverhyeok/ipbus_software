import pandas as pd
import matplotlib.pyplot as plt
import seaborn as sns
from matplotlib.animation import FuncAnimation, PillowWriter
import os

def make_stream_video(stream_csv):
    """Generate GIF of hits accumulating chronologically (EventID)"""
    if not os.path.exists(stream_csv):
        return

    try:
        df = pd.read_csv(stream_csv)
        if df.empty: return

        fig, ax = plt.subplots(figsize=(8, 8))
        ax.set_xlim(15.5, -0.5)
        ax.set_ylim(-0.5, 15.5)

        ax.set_xlabel("Column ID (15 -> 0)")
        ax.set_ylabel("Row ID (15 <- 0)")
        ax.grid(True, linestyle='--', alpha=0.4)

        scat = ax.scatter([], [], s=180, c=[], cmap='plasma', vmin=0, vmax=25, edgecolors='black', alpha=0.7)
        xs, ys, colors = [], [], []

        def update(frame):
            xs.append(df.iloc[frame]["Col"])
            ys.append(df.iloc[frame]["Row"])
            colors.append(df.iloc[frame]["TOA_ns"])

            scat.set_offsets(list(zip(xs, ys)))
            scat.set_array(colors)
            ax.set_title(
                f"Hit Stream Evolution | EventID = {int(df.iloc[frame]['EventID'])} | Current TOA = {colors[-1]:.2f} ns")
            return scat,

        ani = FuncAnimation(fig, update, frames=len(df), interval=150, blit=True)
        base, _ = os.path.splitext(stream_csv)
        out_name = base + "_stream.gif"

        ani.save(out_name, writer=PillowWriter(fps=6))
    except Exception as e:
        pass
    finally:
        plt.close()

def plot_occupancy_hitmap(csv_file):
    """Visualize individual and global 16x16 cumulative occupancy hitmaps"""
    if not os.path.exists(csv_file): return

    try:
        df = pd.read_csv(csv_file, header=None)
        if df.empty or df.shape[1] != 16: return

        df = df.iloc[::-1].reset_index(drop=True)
        df = df.iloc[:, ::-1]

        plt.figure(figsize=(9, 7))
        
        sns.heatmap(df, cmap='viridis', annot=False, cbar_kws={'label': 'Total Hits'},
                    yticklabels=list(range(15, -1, -1)), xticklabels=list(range(15, -1, -1)))

        plt.yticks(rotation=0)
        
        title_prefix = "Global" if "global" in csv_file.lower() else "Run-specific"
        plt.title(f"{title_prefix} Occupancy Hitmap (Bottom-Right = 0,0)\n({os.path.basename(csv_file)})")
        plt.xlabel("Column ID (15 -> 0)")
        plt.ylabel("Row ID (15 -> 0)")

        base, _ = os.path.splitext(csv_file)
        out_name = base + "_plot.png"
        plt.savefig(out_name, dpi=300)
        plt.close()
    except Exception as e:
        pass

def plot_combined_hitmaps(dir_path):
    """Generate a combined 16x16 grid plot by adding Port_L and Port_R data"""
    port_l_file = os.path.join(dir_path, "global_Port_L_hitmap.csv")
    port_r_file = os.path.join(dir_path, "global_Port_R_hitmap.csv")

    if not (os.path.exists(port_l_file) and os.path.exists(port_r_file)):
        return

    try:
        df_l = pd.read_csv(port_l_file, header=None)
        df_r = pd.read_csv(port_r_file, header=None)

        if df_l.shape[1] != 16 or df_r.shape[1] != 16:
            return

        df_combined = df_l + df_r
        df_combined = df_combined.iloc[::-1].reset_index(drop=True).iloc[:, ::-1]

        plt.figure(figsize=(9, 7))

        sns.heatmap(
            df_combined,
            cmap='viridis',
            annot=False,
            cbar_kws={'label': 'Total Hits (Port_L + Port_R)'}, 
            yticklabels=list(range(15, -1, -1)),
            xticklabels=list(range(15, -1, -1))
        )

        plt.yticks(rotation=0)
        plt.title(f"Combined Global Occupancy Hitmap (Port_L + Port_R)\nBottom-Right = 0,0")
        plt.xlabel("Column ID (15 -> 0)")
        plt.ylabel("Row ID (15 -> 0)")

        out_name = os.path.join(dir_path, "global_Combined_hitmap_plot.png")
        plt.savefig(out_name, dpi=300)
        plt.close()

    except Exception as e:
        pass

if __name__ == "__main__":
    target_dir = "results"

    if not os.path.exists(target_dir):
        exit(1)

    stream_files = sorted([f for f in os.listdir(target_dir) if "hit_stream" in f and f.endswith(".csv")])
    if len(stream_files) > 0:
        sample_file = os.path.join(target_dir, stream_files[0])
        make_stream_video(sample_file)

    hitmap_files = sorted([f for f in os.listdir(target_dir) if "hitmap" in f and f.endswith(".csv")])
    for hm_file in hitmap_files:
        plot_occupancy_hitmap(os.path.join(target_dir, hm_file))

    plot_combined_hitmaps(target_dir)
