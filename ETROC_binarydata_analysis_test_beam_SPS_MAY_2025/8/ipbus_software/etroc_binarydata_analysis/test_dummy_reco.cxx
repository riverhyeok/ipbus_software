#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cstdint>
#include <string>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;
using namespace std;

const uint32_t MAGIC_LOG  = 0xAAAA0000;
const uint32_t MAGIC_DAT  = 0xBBBB0000;
const uint32_t MAGIC_META = 0xCCCC0000;

string to_hex_str(uint8_t v) {
    char buf[10];
    snprintf(buf, sizeof(buf), "%02X", v);
    return string(buf);
}

string describe_word(uint64_t w) {
    if ((w >> 39) & 1) return "🔵 HIT (Data)";
    bool is_sync = (((w >> 24) & 0x7FFF) == 0x3C5C);
    if (is_sync) {
        uint8_t t = (w >> 22) & 0x3;
        if (t == 0) return "🟢 HDR (L1: " + to_string((w >> 14) & 0xFF) + ")";
        if (t == 2 || t == 3) return "⚪ FILLER";
    }
    return "🔴 TRL (Hits: " + to_string((w >> 8) & 0xFF) + ", CRC: 0x" + to_hex_str(w & 0xFF) + ")";
}

struct HWDrop {
    uint32_t addr;
    uint32_t count;
    int lap;
    vector<uint32_t> salvaged_payloads;
};

struct SystemStats {
    uint64_t total_32b_words = 0;
    uint64_t total_ram_buffer_empty = 0;
    double elapsed_sec = 0.0;
    double bandwidth_mbps = 0.0;
    uint64_t hw_reported_drops = 0;
    uint64_t total_salvaged_words = 0;
    uint64_t words_injected = 0;
    uint64_t fail_address_drift_orphan = 0;
    uint64_t fail_word_order_quad_slip = 0;
    uint64_t successful_sw_alignments = 0; // 🌟 [추가] 3C5C를 찾아 스스로 복구한 횟수
};

struct ErrorEvent {
    string type;
    size_t index;
    string msg;
};

uint8_t calculate_crc8(const std::vector<uint8_t>& data_bytes) {
    uint8_t crc = 0x00;
    for (uint8_t b : data_bytes) {
        crc ^= b;
        for (int i = 0; i < 8; i++) {
            if (crc & 0x80) crc = (crc << 1) ^ 0x2F;
            else            crc <<= 1;
        }
    }
    return crc;
}

enum FrameType { HEADER, DATA, TRAILER, FILLER, UNKNOWN };

FrameType get_payload_type(uint64_t w) {
    if ((w >> 39) & 1) return DATA;
    bool is_sync = (((w >> 24) & 0x7FFF) == 0x3C5C);
    if (is_sync) {
        uint8_t t = (w >> 22) & 0x3;
        if (t == 0) return HEADER;
        if (t == 2 || t == 3) return FILLER;
    }
    return TRAILER;
}

// 🌟 [추가] 64비트 버퍼 안에서 3C5C를 찾아 비트 정렬 상태(Offset)를 반환하는 함수
int find_3c5c_alignment(uint64_t combined_64b) {
    // 0x3C5C는 15비트, 비트 패턴: 011110001011100
    uint64_t target = 0x3C5C;
    for (int shift = 0; shift <= 24; shift++) {
        if (((combined_64b >> shift) & 0x7FFF) == target) {
            return shift;
        }
    }
    return -1; // 찾지 못함
}

class DummyAnalyzer {
    string name_;
    string current_file_;
    uint64_t total_frames = 0;
    uint64_t headers = 0, datas = 0, trailers = 0, fillers = 0, unknowns = 0;
    
    bool is_synced = false;
    uint64_t words_dropped_at_start = 0;
    bool expect_l1_jump = false;
    uint8_t last_l1_cnt = 0;
    uint64_t l1_jumps = 0;
    uint64_t midstream_truncations = 0;
    
    bool in_event = false;
    std::vector<uint8_t> event_bytes_;
    uint64_t crc_match = 0, crc_mismatch = 0;
    
    vector<uint64_t> word_history;
    vector<ErrorEvent> error_events;

    void push_word_bytes(uint64_t w) {
        for (int i = 4; i >= 0; i--) {
            event_bytes_.push_back((w >> (i * 8)) & 0xFF);
        }
    }

public:
    DummyAnalyzer(const string& name) : name_(name) {
        word_history.reserve(60000000);
    }
    
    void set_file(const string& fn) { current_file_ = fn; }
    
    void force_resync() {
        if (in_event) {
            midstream_truncations++;
            size_t current_idx = total_frames > 0 ? total_frames - 1 : 0;
            error_events.push_back({"MIDSTREAM CUT", current_idx, "Forced Resync (Hardware Slip or Payload Drop)"});
        }
        in_event = false;
        is_synced = false;
        expect_l1_jump = false;
    }
    
    void process_word(uint64_t w) {
        total_frames++;
        word_history.push_back(w);
        size_t current_idx = total_frames - 1;
        FrameType type = get_payload_type(w);
        
        if (!is_synced) {
            if (type == HEADER) {
                is_synced = true;
                in_event = true;
                headers++;
                last_l1_cnt = (w >> 14) & 0xFF;
                event_bytes_.clear();
                push_word_bytes(w);
            } else {
                words_dropped_at_start++;
            }
            return;
        }
        
        if (type == HEADER) {
            headers++;
            if (in_event) {
                midstream_truncations++;
                error_events.push_back({"MIDSTREAM CUT", current_idx, "Missing Trailer. New Header overwritten."});
                expect_l1_jump = true;
            }
            in_event = true;
            uint8_t l1_cnt = (w >> 14) & 0xFF;
            if (!expect_l1_jump) {
                uint8_t expected_l1 = (last_l1_cnt + 1) & 0xFF;
                if (l1_cnt != expected_l1) {
                    l1_jumps++;
                    error_events.push_back({"L1 JUMP", current_idx, "Expected: " + to_string((int)expected_l1) + ", Received: " + to_string((int)l1_cnt)});
                }
            }
            expect_l1_jump = false;
            last_l1_cnt = l1_cnt;
            event_bytes_.clear();
            push_word_bytes(w);
            
        } else if (type == DATA) {
            datas++;
            if (in_event) push_word_bytes(w);
            else unknowns++;
            
        } else if (type == TRAILER) {
            trailers++;
            if (in_event) {
                uint8_t received_crc = w & 0xFF;
                for (int i = 4; i >= 1; i--) {
                    event_bytes_.push_back((w >> (i * 8)) & 0xFF);
                }
                uint8_t expected_crc = calculate_crc8(event_bytes_);
                if (expected_crc == received_crc) {
                    crc_match++;
                } else {
                    crc_mismatch++;
                    error_events.push_back({"CRC MISMATCH", current_idx, "Expected: 0x" + to_hex_str(expected_crc) + ", Received: 0x" + to_hex_str(received_crc)});
                }
                in_event = false;
            } else {
                unknowns++;
            }
            
        } else if (type == FILLER) {
            fillers++;
        } else {
            unknowns++;
        }
    }
    
    void finalize_stream() {
        if (in_event) in_event = false;
    }
    
    void print_summary(const SystemStats& sys) const {
        cout << "\n=================================================";
        cout << "\n 📊 [ " << name_ << " CHANNEL DUMMY DATA REPORT ]";
        cout << "\n=================================================\n";
        if (total_frames == 0) {
            cout << " ⚠️ No data received.\n";
            return;
        }
        cout << " 1️⃣ FRAME RATIO ANALYSIS (Total 40b Words: " << total_frames << ")\n";
        cout << "   - Headers : " << headers  << " (" << fixed << setprecision(1) << (100.0 * headers / total_frames) << "%)\n";
        cout << "   - Data    : " << datas    << " (" << fixed << setprecision(1) << (100.0 * datas / total_frames) << "%)\n";
        cout << "   - Trailers: " << trailers << " (" << fixed << setprecision(1) << (100.0 * trailers / total_frames) << "%)\n";
        cout << "   - Fillers : " << fillers  << " (" << fixed << setprecision(1) << (100.0 * fillers / total_frames) << "%)\n";
        
        cout << "\n 2️⃣ BOUNDARY & CONTINUITY (Robust Sync)\n";
        cout << "   - Filtered at Start   : " << words_dropped_at_start << " words (Normal)\n";
        cout << "   - Mid-stream Cut-offs : " << midstream_truncations << " times (Chunk boundary drops)\n";
        cout << "   - Real L1 Jumps       : " << l1_jumps << " times (Unexplained data loss)\n";
        
        if (l1_jumps == 0 && crc_mismatch == 0 && midstream_truncations == 0) {
            cout << "   - Status : PERFECT ✅ (Zero Data Loss)\n";
        } else {
            cout << "   - Status : DATA LOSS DETECTED ❌ (Review logs below)\n";
        }
        
        cout << "\n 3️⃣ CRC-8 VERIFICATION (LUT Based Full-Frame Check)\n";
        cout << "   - Matched (Pass) : " << crc_match << " events\n";
        cout << "   - Mismatched     : " << crc_mismatch << " events\n";
        
        if (!error_events.empty()) {
            cout << "\n 4️⃣ 🕵️‍♂️ ERROR CONTEXT DUMP (±10 Words)\n";
            int count = 0;
            for (const auto& err : error_events) {
                if (count++ >= 20) break; // 최대 20개까지만 출력
                cout << "  -------------------------------------------------\n";
                cout << "  🔥 ERROR: " << err.type << " | " << err.msg << " (At Word Index: " << err.index + 1 << ")\n";
                int start = max(0, (int)err.index - 10);
                int end   = min((int)word_history.size() - 1, (int)err.index + 10);
                for (int i = start; i <= end; i++) {
                    string prefix = (i == (int)err.index) ? "  🩸 => " : "        ";
                    cout << prefix << "[" << setw(8) << i + 1 << "] 0x" << hex << uppercase << setfill('0') << setw(10) << word_history[i] << dec << setfill(' ') << " | " << describe_word(word_history[i]) << "\n";
                }
            }
        }
        cout << "=================================================\n";
    }
};

int main() {
    string folder = "daq_bin_results";
    vector<string> files;
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin") files.push_back(entry.path().filename().string());
        }
    }
    sort(files.begin(), files.end());
    
    if (files.empty()) {
        cout << "❌ No .bin files found in daq_bin_results/\n";
        return 1;
    }
    
    for (const string& fn : files) {
        ifstream f(folder + "/" + fn, ios::binary);
        if (!f.is_open()) continue;
        
        cout << "\n>>> 🚀 Validating Dummy Data: " << fn << " ...\n";
        DummyAnalyzer right_ch("RIGHT");
        DummyAnalyzer left_ch("LEFT");
        
        right_ch.set_file(fn);
        left_ch.set_file(fn);
        
        SystemStats sys;
        uint32_t magic, size_words;
        uint64_t global_valid_cnt = 0;
        vector<HWDrop> pending_drops;
        
        // 🌟 Bitslip 소프트웨어 정렬을 위한 상태 머신 변수들
        vector<uint32_t> valid_quad;
        int current_bit_offset_r = 24; // W1(24b)와 W2(16b) 조합 시 기본 정상 오프셋
        int current_bit_offset_l = 24; 
        
        uint64_t leftover_bits_r = 0;
        uint64_t leftover_bits_l = 0;
        
        auto process_quad_word = [&](uint32_t w) {
            uint32_t marker = (w >> 29) & 0x3;
            
            // Quad 순서가 정상일 때 조립
            if (marker == valid_quad.size()) {
                valid_quad.push_back(w);
                if (valid_quad.size() == 4) {
                    uint64_t raw_right = ((uint64_t)(valid_quad[1] & 0xFFFFu) << 24) | (uint64_t)(valid_quad[0] & 0xFFFFFFu);
                    uint64_t raw_left  = ((uint64_t)(valid_quad[3] & 0xFFFFu) << 24) | (uint64_t)(valid_quad[2] & 0xFFFFFFu);
                    
                    // 🌟 3C5C 패턴 능동적 서치 (Auto Re-alignment)
                    // 현재 오프셋으로 추출했는데 헤더(3C5C)가 보이지 않는다면 주변 비트 서치
                    int found_offset_r = find_3c5c_alignment(raw_right);
                    if (found_offset_r != -1 && found_offset_r != current_bit_offset_r) {
                        current_bit_offset_r = found_offset_r;
                        sys.successful_sw_alignments++;
                    }
                    
                    int found_offset_l = find_3c5c_alignment(raw_left);
                    if (found_offset_l != -1 && found_offset_l != current_bit_offset_l) {
                        current_bit_offset_l = found_offset_l;
                        sys.successful_sw_alignments++;
                    }
                    
                    // 보정된 오프셋을 적용하여 40비트 데이터 온전하게 복원
                    uint64_t d_right, d_left;
                    if (current_bit_offset_r == 24) { d_right = raw_right; } 
                    else { d_right = (raw_right >> (24 - current_bit_offset_r)) & 0xFFFFFFFFFFULL; }
                    
                    if (current_bit_offset_l == 24) { d_left = raw_left; } 
                    else { d_left = (raw_left >> (24 - current_bit_offset_l)) & 0xFFFFFFFFFFULL; }
                    
                    right_ch.process_word(d_right);
                    left_ch.process_word(d_left);
                    valid_quad.clear();
                }
            } else {
                // 🌟 마커 어긋남 발생 (Slip) -> 버리지 않고 강제 동기화 후 새 마커부터 다시 담기
                if (valid_quad.size() == 1 && valid_quad[0] == 0 && marker == 0) {
                    sys.total_ram_buffer_empty++;
                    valid_quad[0] = w;
                    return;
                }
                sys.fail_word_order_quad_slip++;
                valid_quad.clear();
                right_ch.force_resync();
                left_ch.force_resync();
                
                if (marker == 0) valid_quad.push_back(w);
            }
        };

        // 🌟 물리 주소 262144 (2^18) 적용
        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (magic == MAGIC_LOG) {
                vector<uint32_t> logs(size_words); f.read((char*)logs.data(), size_words * 4);
                if (logs[0] == 0xDEADBEEF) continue;
                
                const uint32_t ADDR_WIDTH = 12;
                const uint32_t ADDR_MASK = (1 << ADDR_WIDTH) - 1;
                uint32_t valid_count = logs[0] & ADDR_MASK;
                uint32_t limit = min((uint32_t)logs.size(), valid_count + 1);
                
                vector<uint32_t> temp_payloads;
                for (uint32_t i = 1; i < limit; i++) {
                    uint32_t lw = logs[i];
                    if ((lw >> 31) == 1) temp_payloads.push_back(lw);
                    else if ((lw >> 30) == 0) {
                        uint32_t drops = (lw >> 17) & 0x1FFF;
                        uint32_t addr = lw & 0x1FFFF;
                        sys.hw_reported_drops += drops;
                        sys.total_salvaged_words += temp_payloads.size();
                        pending_drops.push_back({addr, drops, -1, temp_payloads});
                        temp_payloads.clear();
                    } else {
                        int lap = (lw & 0x7) == 0 ? 7 : (lw & 0x7) - 1;
                        for (auto& d : pending_drops) { if (d.lap == -1) d.lap = lap; }
                    }
                }
            } else if (magic == MAGIC_DAT) {
                vector<uint32_t> d(size_words); f.read((char*)d.data(), size_words * 4);
                sys.total_32b_words += size_words;
                
                for (uint32_t w : d) {
                    // 🌟 262,144 물리 주소 동기화
                    uint32_t phys_addr = global_valid_cnt % 262144;
                    int cur_lap = (global_valid_cnt / 262144) % 8;
                    global_valid_cnt++;
                    
                    for (auto it = pending_drops.begin(); it != pending_drops.end(); ) {
                        if (it->addr == phys_addr && (it->lap == cur_lap || it->lap == -1)) {
                            sys.words_injected += it->salvaged_payloads.size();
                            for (uint32_t sw : it->salvaged_payloads) process_quad_word(sw);
                            it = pending_drops.erase(it);
                            break;
                        } else {
                            ++it;
                        }
                    }
                    process_quad_word(w);
                }
            } else if (magic == MAGIC_META) {
                f.read((char*)&sys.elapsed_sec, 8);
                sys.bandwidth_mbps = (sys.total_32b_words * 32.0) / 1000000.0 / sys.elapsed_sec;
            } else break;
        }
        
        right_ch.finalize_stream(); left_ch.finalize_stream();
        for (const auto& pd : pending_drops) sys.fail_address_drift_orphan += pd.salvaged_payloads.size();
        
        cout << "\n  =======================================================\n";
        cout << "  📡 [ LOG INJECTION & RECOVERY STATS ]\n";
        cout << "   ├─ HW Drops Reported: " << sys.hw_reported_drops << " words\n";
        cout << "   ├─ ✅ Successful Inj: " << sys.words_injected << " words\n";
        cout << "   ├─ ❌ Failed Recovery: Drift=" << sys.fail_address_drift_orphan << ", Slip=" << sys.fail_word_order_quad_slip << "\n";
        cout << "   └─ 🛠️  SW Re-alignments: " << sys.successful_sw_alignments << " times (3C5C Phase Lock)\n";
        cout << "  =======================================================\n";
        
        right_ch.print_summary(sys);
        left_ch.print_summary(sys);
    }
    return 0;
}