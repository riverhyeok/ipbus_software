import numpy as np
import lecroyparser
import matplotlib.pyplot as plt

# 1. 파일 경로 설정
trc_file = "/home/jhsong/root/C2--Trace119192.trc"

# JSON Configuration에서 추출한 파라미터 (SPS May 2025 Test Beam)
NUM_SEGMENTS = 5000
WINDOW_NS = 25.0  # nanoseconds
SAMPLE_RATE = 20e9 # 20 GS/s

try:
    print(f"Reading Sequence Mode TRC file: {trc_file} ...")
    data = lecroyparser.ScopeData(trc_file)
    y_raw = np.array(data.y)
    total_points = len(y_raw)
    
    print("\n================ [ Sequence TRC 파싱 결과 ] ================")
    print(f"TRC 내부 총 데이터 포인트: {total_points:,} 개")
    
    # 2. 데이터 세그먼트 분할
    # 이론적 포인트: 20 GS/s * 25 ns = 500 points/segment
    # 오실로스코프 헤더 패딩 등으로 인해 실제와 오차가 있을 수 있으므로 동적 계산
    points_per_seg = total_points // NUM_SEGMENTS
    
    print(f"예상 세그먼트 수: {NUM_SEGMENTS} 개")
    print(f"세그먼트 당 포인트 수: {points_per_seg} 개")
    
    # 정확히 5000개의 세그먼트로 나눌 수 있게 배열 자르기 및 2D 배열로 형태 변환
    y_valid = y_raw[:points_per_seg * NUM_SEGMENTS]
    y_segments = y_valid.reshape((NUM_SEGMENTS, points_per_seg))
    
    # 시간 축 생성 (0 ~ 25 ns)
    time_axis_ns = np.linspace(0, WINDOW_NS, points_per_seg)

    print(f"데이터 Reshape 완료: {y_segments.shape}")
    print("============================================================")

    # 3. 데이터 시각화 (Overlay Display 모드 모방)
    plt.figure(figsize=(10, 6))
    
    # 5000개를 다 그리면 너무 무겁고 보이지 않으므로, 처음 200개의 이벤트만 투명도를 주어 겹쳐 그립니다.
    plot_count = min(10, NUM_SEGMENTS)
    for i in range(plot_count):
        plt.plot(time_axis_ns, y_segments[i], color='royalblue', alpha=0.1, linewidth=1)
    
    # 평균 파형(Average Trace)을 굵은 빨간 선으로 추가
    y_mean = np.mean(y_segments, axis=0)
    plt.plot(time_axis_ns, y_mean, color='red', linewidth=2, label='Average Trace')

    plt.title(f'Oscilloscope Channel 2 (Clock) - {plot_count} Segments Overlaid')
    plt.xlabel('Time (ns)')
    plt.ylabel('Voltage (V)')
    
    # JSON 채널 2 설정 (lower: -1, upper: 2) 반영
    plt.ylim(-1.0, 2.0)
    plt.grid(True, linestyle='--', alpha=0.5)
    plt.legend()
    plt.tight_layout()
    
    output_img = "trace_c2_sequence_overlay.png"
    plt.savefig(output_img, dpi=300)
    print(f"\n성공적으로 분석되었습니다. 오버레이 파형이 '{output_img}'로 저장되었습니다.")

except Exception as e:
    print(f"파일을 파싱하는 중 오류가 발생했습니다: {e}")
