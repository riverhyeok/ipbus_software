#!/bin/bash

# 1. Git 소유권 문제 해결 및 꼬인 Staging Area 초기화
git config --global --add safe.directory $(pwd)
git reset >/dev/null 2>&1

# 2. 대용량 파일 Git 추적 제외 (속도 저하 원인 제거)
cat << 'EOF' > .gitignore
*.bit
*.ltx
*.tgz
*.tar.gz
*.csv
*.png
*.bin
daq_bin_results/
results/
pseudo_diag_results/
autossh-*/
EOF

# 3. 불필요한 파일 삭제 (순수 리눅스 명령어로 즉시 제거)
FILES_TO_REMOVE=(
    "debug_traffic.py"
    "my_tutorial.py"
    "test.py"
    "test_dpram.cxx"
    "test_dummy_reco.cxx"
    "test_dummy_reco"
    "tutorial.py"
)
for file in "${FILES_TO_REMOVE[@]}"; do
    rm -rf "$file" 2>/dev/null
done

# 4. 폴더명 변경
if [ -d "etroc_binarydata_analysis" ]; then
    mv etroc_binarydata_analysis etroc_binarydata_KNU_8_13_analysis 2>/dev/null
fi

# 5. 타겟 폴더 생성 및 기존 README 병합
TARGET_DIR="ETROC_binarydata_analysis_test_beam_SPS_MAY_2025"
mkdir -p "$TARGET_DIR"

if [ -f "README.md" ]; then
    echo "Unpacking ETROC binary data (JunHyeok Song, Jul 24, 2026)" > "$TARGET_DIR/README.md"
    echo "https://indico.cern.ch/event/1712925/" >> "$TARGET_DIR/README.md"
    echo "" >> "$TARGET_DIR/README.md"
    cat README.md >> "$TARGET_DIR/README.md"
    rm README.md
fi

# 6. 핵심 파일 제외 나머지 이동
for item in *; do
    if [ ! -e "$item" ]; then continue; fi

    if [[ "$item" == "$TARGET_DIR" || \
          "$item" == "etroc_binarydata_KNU_8_13_analysis" || \
          "$item" == "test_bandwidth.cxx" || \
          "$item" == "test_bandwidth_reco.cxx" || \
          "$item" == "ipbus_example.xml" || \
          "$item" == "connections.xml" || \
          "$item" == "hitmap.py" || \
          "$item" == "Makefile" || \
          "$item" == ".gitignore" || \
          "$item" == ".git" || \
          "$item" == *.sh ]]; then
        continue
    fi
    
    mv "$item" "$TARGET_DIR/" 2>/dev/null
done

# 7. 루트 디렉토리 새 README.md 작성
cat << 'EOF' > README.md
# ipbus_software

The analysis for the testbeam data is located in the `ETROC_binarydata_analysis_test_beam_SPS_MAY_2025` folder.

The software in this repository includes the following files used for DAQ at KNU:
- `test_bandwidth.cxx`
- `ipbus_example.xml`
- `connections.xml`

The file used for data analysis is:
- `test_bandwidth_reco.cxx`

## Reference
- [ETROC2 Reference Manual 0.41](https://indico.cern.ch/event/1288660/contributions/5415154/attachments/2651263/4590830/ETROC2_Reference_Manual%200.41.pdf)
EOF

# 8. hitmap.py 영문 주석 버전으로 재작성
cat << 'EOF' > hitmap.py
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
EOF

# 9. C++ 코드 주석 영어로 변환 (sed)
sed -i 's/비트 시프트 완벽 적용/Bit shift perfectly applied/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/IPbus 패킷 증발 방어 & 자동 재시도 적용/IPbus packet loss defense & auto-retry applied/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/커맨드 전송 (최대 3회 재시도)/Command transmission (Max 3 retries)/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/성공하면 탈출/Break on success/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/UDP 패킷 씹힘! 2ms 쉬고 재시도/UDP packet dropped! Wait 2ms and retry/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/3번 다 실패하면 I2C 실패로 처리/Treat as I2C failure if all 3 attempts fail/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/폴링(상태 확인) 루프/Polling (status check) loop/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/읽는 중에 UDP 패킷이 씹히면? 당황하지 않고 2ms 대기 후 루프 재진입 (다시 읽기)/If UDP packet is dropped during read, wait 2ms and re-enter loop (read again)/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/주석 해제 및 변수 활성화!/Uncommented and variable enabled!/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/메뉴 옵션 7번 추가/Added menu option 7/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/7번에서 8번으로 밀림/Shifted from 7 to 8/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/8번에서 9번으로 밀림/Shifted from 8 to 9/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/1번부터 7번까지 입력 가능하도록 수정/Modified to allow inputs from 1 to 7/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/18번째 비트에 1비트 마스킹 쓰기 로직 추가/Added 1-bit masking write logic to the 18th bit/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/8번에서 6번으로 이동/Moved from 8 to 6/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/6번에서 7번으로 이동/Moved from 6 to 7/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/7번에서 8번으로 이동/Moved from 7 to 8/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/문구 변경됨/Text changed/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/6번 (기존 8번):/Option 6 (Formerly 8):/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/7번 (기존 6번):/Option 7 (Formerly 6):/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/8번 (기존 7번):/Option 8 (Formerly 7):/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/9번: 문구 업데이트됨/Option 9: Text updated/g' test_bandwidth.cxx 2>/dev/null
sed -i 's/10번: 종료/Option 10: Exit/g' test_bandwidth.cxx 2>/dev/null

sed -i 's/표준편차 계산을 위한 sqrt 함수 포함/Include sqrt function for standard deviation calculation/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/L1 점프 통계 변수/L1 jump statistics variables/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/Counter A 점프 통계 변수/Counter A jump statistics variables/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/복구된 에러 상세 추적 변수들/Recovered error detail tracking variables/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/디바운스를 위한 이전 단어 기억 변수/Previous word memory variable for debouncing/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/\[핵심 소프트웨어 방어막\] Hardware Stutter(복제 버그) 원천 차단!/\[Core Software Shield\] Completely block Hardware Stutter (duplication bug)!/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/방금 처리한 단어와 토씨 하나 안 틀리고 100% 똑같다면,/If it is 100% identical to the just processed word,/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/이는 ETROC2가 보낸 것이 아니라 FPGA 클럭 엇박자로 복제된 쓰레기입니다./it is not sent by ETROC2, but a duplicated garbage due to FPGA clock mismatch./g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/깔끔하게 무시!/Ignore it cleanly!/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/현재 단어 기억/Remember current word/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/채널 쪼개기용 임시 조립 버퍼 (루프를 넘어 상태가 유지되어야 함)/Temporary assembly buffer for channel splitting (state must be maintained across loops)/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/스마트 라우팅 워드 프로세서 (4-word Quad 종속성 완전 제거)/Smart routing word processor (completely removed 4-word Quad dependency)/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/1. 상태 레지스터(Drop \/ Lap)는 실제 Payload 데이터가 아니므로 스킵/1. Status registers (Drop \/ Lap) are not actual Payload data, so skip them/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/2. 데이터 워드 (MSB == 1), 최상위 3비트로 채널 분기/2. Data word (MSB == 1), branch channel by top 3 bits/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/🔴 Right 채널 라우팅/🔴 Right channel routing/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/High가 짝 없이 들어온 경우 에러 플래그 처리 후 해당 채널만 동기화 리셋/If High arrives without a pair, process error flag and reset synchronization for this channel only/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/🔵 Left 채널 라우팅/🔵 Left channel routing/g' test_bandwidth_reco.cxx 2>/dev/null
sed -i 's/식별자 오류/Identifier error/g' test_bandwidth_reco.cxx 2>/dev/null

# 10. 빠른 Git 커밋 (.gitignore 적용)
git add .
git commit -m "Restructure repository, remove unused files, and translate comments to English"
