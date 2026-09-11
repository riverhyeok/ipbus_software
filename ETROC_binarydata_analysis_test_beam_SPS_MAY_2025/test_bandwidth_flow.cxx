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

namespace etroc {
    static const uint64_t SYNC = 0x3C5C;
    inline bool is_data(uint64_t d)    { return  (d >> 39) & 1; }
    inline bool is_sync(uint64_t d)    { return !is_data(d) && (((d >> 24) & 0x7FFF) == SYNC); }
    inline bool is_header(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x0); }
    inline bool is_filler(uint64_t d)  { return  is_sync(d) && (((d >> 22) & 0x3) == 0x2); }
    inline bool is_trailer(uint64_t d) { return !is_data(d) && !is_sync(d); }
    inline uint16_t trl_hits(uint64_t d) { return (d >>  8) & 0xFF; }

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
}

class StutterHypothesisTester {
    string name_;
    
    // 🌟 가설 검증을 위한 통계 카운터 🌟
    uint64_t total_events_checked = 0;
    
    uint64_t total_crc_errors = 0;
    uint64_t crc_fixed_by_stutter_removal = 0; // 복제 단어 제거 시 CRC가 0이 되는 에러
    
    uint64_t total_orphan_trailers = 0;
    uint64_t orphan_trailer_is_stutter = 0; // 고아 트레일러가 직전 워드의 복제본인 경우
    
    uint64_t total_overlapping_headers = 0;
    uint64_t overlapping_header_is_stutter = 0; // 중복 헤더가 직전 워드의 복제본인 경우

    bool in_event_ = false;
    uint64_t last_word_ = 0xFFFFFFFFFFFFFFFFULL;
    vector<uint64_t> current_event_words_;

    void push_word_bytes(std::vector<uint8_t>& target, uint64_t w) {
        for (int i = 4; i >= 0; i--) target.push_back((w >> (i * 8)) & 0xFF);
    }

    // 🌟 [핵심 로직] 에러가 난 이벤트를 받아, 복제본을 제거하고 복원되는지 테스트 🌟
    bool can_stutter_fix_event(const vector<uint64_t>& raw_words) {
        if (raw_words.empty()) return false;
        
        vector<uint64_t> dedup_words;
        dedup_words.push_back(raw_words[0]); // 헤더는 무조건 삽입
        
        // 연속된 복제 단어(Stutter) 필터링
        for (size_t i = 1; i < raw_words.size(); ++i) {
            if (raw_words[i] != raw_words[i-1]) {
                dedup_words.push_back(raw_words[i]);
            }
        }
        
        // 필터링 후 구조가 정상인지 확인 (최소 2단어 이상, 마지막은 트레일러)
        if (dedup_words.size() < 2) return false;
        uint64_t trl = dedup_words.back();
        if (!etroc::is_trailer(trl)) return false;
        
        // Hit 개수가 일치하는지 확인
        if (etroc::trl_hits(trl) != (dedup_words.size() - 2)) return false;
        
        // CRC 재계산
        vector<uint8_t> dedup_bytes;
        for (uint64_t dw : dedup_words) push_word_bytes(dedup_bytes, dw);
        
        return (etroc::calculate_crc8(dedup_bytes) == 0x00);
    }

public:
    StutterHypothesisTester(const string& name) : name_(name) {}

    void process_word(uint64_t w) {
        bool is_exact_duplicate = (w == last_word_);

        if (etroc::is_header(w)) {
            if (in_event_) {
                // 이벤트 중인데 헤더가 또 옴 (WP_Boundary_Cuts 발생 조건)
                total_overlapping_headers++;
                if (is_exact_duplicate) overlapping_header_is_stutter++;
            }
            in_event_ = true;
            current_event_words_.clear();
            current_event_words_.push_back(w);
            last_word_ = w;
            return;
        }

        if (etroc::is_trailer(w)) {
            if (!in_event_) {
                // 헤더가 없는데 트레일러가 옴 (Events_Missing_Header 발생 조건)
                total_orphan_trailers++;
                if (is_exact_duplicate) orphan_trailer_is_stutter++;
                last_word_ = w;
                return;
            }
            
            // 정상적인 트레일러 도착 - 이벤트 닫기
            current_event_words_.push_back(w);
            total_events_checked++;
            
            // 기존 CRC 계산
            vector<uint8_t> raw_bytes;
            for (uint64_t dw : current_event_words_) push_word_bytes(raw_bytes, dw);
            
            if (etroc::calculate_crc8(raw_bytes) != 0x00) {
                total_crc_errors++;
                // 🌟 가설 검증: 복제 단어를 지우면 CRC가 정상으로 돌아오는가?
                if (can_stutter_fix_event(current_event_words_)) {
                    crc_fixed_by_stutter_removal++;
                }
            }
            in_event_ = false;
            last_word_ = w;
            return;
        }

        if (etroc::is_data(w)) {
            if (in_event_) {
                current_event_words_.push_back(w);
            }
            last_word_ = w;
            return;
        }
        
        last_word_ = w;
    }

    void print_hypothesis_result() const {
        cout << "\n======================================================\n";
        cout << " 🔬 [" << name_ << "] HYPOTHESIS TEST RESULT (THE STUTTER BUG)\n";
        cout << "======================================================\n";
        
        double crc_fix_pct = total_crc_errors > 0 ? (double)crc_fixed_by_stutter_removal / total_crc_errors * 100.0 : 0;
        double orphan_fix_pct = total_orphan_trailers > 0 ? (double)orphan_trailer_is_stutter / total_orphan_trailers * 100.0 : 0;
        double overlap_fix_pct = total_overlapping_headers > 0 ? (double)overlapping_header_is_stutter / total_overlapping_headers * 100.0 : 0;

        cout << " 1️⃣ CRC Mismatch Analysis\n";
        cout << "   - Total CRC Errors: " << total_crc_errors << "\n";
        cout << "   - Explained by Stuttering (Duplicated Words): " << crc_fixed_by_stutter_removal << " \n";
        cout << "   - Match Rate: " << fixed << setprecision(2) << crc_fix_pct << " %  " 
             << (crc_fix_pct > 99.0 ? "✅ HYPOTHESIS CONFIRMED" : "❌ OTHER CAUSES EXIST") << "\n\n";

        cout << " 2️⃣ Missing Header (Orphan Trailer) Analysis\n";
        cout << "   - Total Orphan Trailers: " << total_orphan_trailers << "\n";
        cout << "   - Explained by Stuttering (Duplicated Trailer): " << orphan_trailer_is_stutter << "\n";
        cout << "   - Match Rate: " << fixed << setprecision(2) << orphan_fix_pct << " %  "
             << (orphan_fix_pct > 99.0 ? "✅ HYPOTHESIS CONFIRMED" : "❌ OTHER CAUSES EXIST") << "\n\n";

        cout << " 3️⃣ WP Boundary Cuts (Overlapping Header) Analysis\n";
        cout << "   - Total Overlapping Headers: " << total_overlapping_headers << "\n";
        cout << "   - Explained by Stuttering (Duplicated Header): " << overlapping_header_is_stutter << "\n";
        cout << "   - Match Rate: " << fixed << setprecision(2) << overlap_fix_pct << " %  "
             << (overlap_fix_pct > 99.0 ? "✅ HYPOTHESIS CONFIRMED" : "❌ OTHER CAUSES EXIST") << "\n";
        cout << "======================================================\n";
    }
};

int main() {
    string folder = "daq_bin_results";
    vector<string> files;
    
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin")
                files.push_back(entry.path().filename().string());
        }
    }
    
    if (files.empty()) return 1;
    // 통계가 가장 많았던 파일(가장 최근) 하나를 타겟으로 분석
    string target_file = files.back(); 
    
    cout << "\n>>> 🚀 Running Strict Hypothesis Verification on: " << target_file << " ...\n";
    ifstream f(folder + "/" + target_file, ios::binary);
    if (!f.is_open()) return 1;
    
    StutterHypothesisTester right_ch("RIGHT_CHANNEL");
    StutterHypothesisTester left_ch("LEFT_CHANNEL");
    
    uint32_t magic, size_words;
    uint64_t right_low = 0, left_low = 0;
    bool right_low_ready = false, left_low_ready = false;

    while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
        if (magic == MAGIC_LOG || magic == MAGIC_META) {
            f.seekg(size_words * 4, ios::cur); // Log와 Meta는 건너뜀
            continue;
        }
        
        if (magic == MAGIC_DAT) {
            vector<uint32_t> d(size_words);
            f.read((char*)d.data(), size_words * 4);
            
            for (uint32_t w : d) {
                if (w == 0 || (w >> 31) == 0) continue; 
                
                uint8_t channel_and_part = (w >> 29) & 0x7; 
                switch (channel_and_part) {
                    case 0b100: right_low = w & 0xFFFFFF; right_low_ready = true; break;
                    case 0b101: 
                        if (right_low_ready) {
                            right_ch.process_word(((uint64_t)(w & 0xFFFF) << 24) | right_low);
                            right_low_ready = false;
                        } break;
                    case 0b110: left_low = w & 0xFFFFFF; left_low_ready = true; break;
                    case 0b111: 
                        if (left_low_ready) {
                            left_ch.process_word(((uint64_t)(w & 0xFFFF) << 24) | left_low);
                            left_low_ready = false;
                        } break;
                }
            }
        }
    }
    
    right_ch.print_hypothesis_result();
    left_ch.print_hypothesis_result();
    
    return 0;
}