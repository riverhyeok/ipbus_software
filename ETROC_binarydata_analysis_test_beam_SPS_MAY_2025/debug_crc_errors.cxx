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
    
    inline uint32_t hdr_bcid(uint64_t d)  { return  d        & 0xFFF; } 
    inline uint8_t  hdr_type(uint64_t d)  { return (d >> 12) & 0x3;   } 
    inline uint8_t  hdr_l1_cnt(uint64_t d){ return (d >> 14) & 0xFF;  } 
    
    inline uint8_t  dat_ea(uint64_t d)    { return (d >> 37) & 0x3;   } 
    inline uint8_t  dat_col1(uint64_t d)  { return (d >> 33) & 0xF;   } 
    inline uint8_t  dat_row1(uint64_t d)  { return (d >> 29) & 0xF;   } 
    inline uint8_t  dat_col2(uint64_t d)  { return (d >> 25) & 0xF;   } 
    inline uint8_t  dat_row2(uint64_t d)  { return (d >> 21) & 0xF;   } 
    inline uint32_t dat_bcid(uint64_t d)  { return (d >>  9) & 0xFFF; } 
    
    inline uint8_t  trl_status(uint64_t d) { return (d >> 16) & 0x3F;   } 
    inline uint16_t trl_hits(uint64_t d)   { return (d >>  8) & 0xFF;   } 
    inline uint32_t trl_chipid(uint64_t d) { return (d >> 22) & 0x1FFFF;} 

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

inline void unpack_quad(uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, 
                        uint64_t& d_right, uint8_t& type_right, 
                        uint64_t& d_left,  uint8_t& type_left) {
    d_right    = ((uint64_t)(w2 & 0xFFFFu) << 24) | (uint64_t)(w1 & 0xFFFFFFu);
    type_right = (w2 >> 16) & 0x3;
    d_left     = ((uint64_t)(w4 & 0xFFFFu) << 24) | (uint64_t)(w3 & 0xFFFFFFu);
    type_left  = (w4 >> 16) & 0x3;
}

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };

FrameType get_payload_type(uint64_t w) {
    if (etroc::is_data(w)) return DATA;
    if (etroc::is_header(w)) return HEADER;
    if (etroc::is_filler(w)) return FILLER;
    if (etroc::is_trailer(w)) return TRAILER;
    return UNKNOWN;
}

struct HWDrop { 
    uint32_t addr; uint32_t count; int lap; vector<uint32_t> salvaged_payloads; 
};

// Global max count
int max_crc_errors_to_print = 5;
int current_crc_errors = 0;

class EtrocChannelDebug {
    string name_; 
    uint32_t expected_chip_id_;
    bool in_event_ = false; 
    uint32_t hdr_bcid_ = 0; 
    
    uint64_t header_word_ = 0;
    std::vector<uint64_t> current_event_data_; 
    std::vector<uint8_t> event_bytes_;         

    void push_word_bytes(uint64_t w, int byte_count = 5) {
        for (int i = byte_count - 1; i >= 0; i--) event_bytes_.push_back( (w >> (i * 8)) & 0xFF );
    }

    void print_event_dump(uint64_t trailer_word, uint8_t expected_crc, uint8_t actual_crc) {
        cout << "\n======================================================\n";
        cout << "🚨 [" << name_ << "] CRC ERROR DETECTED! (Dump " << (current_crc_errors + 1) << "/" << max_crc_errors_to_print << ")\n";
        cout << "Calculated CRC: 0x" << hex << setfill('0') << setw(2) << (int)expected_crc 
             << " | Trailer CRC: 0x" << setw(2) << (int)actual_crc << dec << setfill(' ') << "\n";
        
        cout << "------------------------------------------------------\n";
        cout << " HEADER  : 0x" << hex << setfill('0') << setw(10) << header_word_ << dec << setfill(' ') 
             << " | BCID: " << etroc::hdr_bcid(header_word_) 
             << " | L1_CNT: " << (int)etroc::hdr_l1_cnt(header_word_) << "\n";
             
        for (size_t i = 0; i < current_event_data_.size(); i++) {
            uint64_t dw = current_event_data_[i];
            cout << " DATA[" << setw(2) << i << "]: 0x" << hex << setfill('0') << setw(10) << dw << dec << setfill(' ')
                 << " | BCID: " << etroc::dat_bcid(dw) 
                 << " | R1:C1: " << (int)etroc::dat_row1(dw) << ":" << (int)etroc::dat_col1(dw)
                 << " | R2:C2: " << (int)etroc::dat_row2(dw) << ":" << (int)etroc::dat_col2(dw)
                 << " | EA: " << (int)etroc::dat_ea(dw) << "\n";
        }
        
        cout << " TRAILER : 0x" << hex << setfill('0') << setw(10) << trailer_word << dec << setfill(' ')
             << " | HITS: " << etroc::trl_hits(trailer_word) 
             << " | STATUS: 0x" << hex << (int)etroc::trl_status(trailer_word) << dec 
             << " | CHIP_ID: 0x" << hex << etroc::trl_chipid(trailer_word) << dec << "\n";
             
        cout << "------------------------------------------------------\n";
        cout << " Event Bytes (" << event_bytes_.size() << " bytes used for CRC): ";
        for (size_t i = 0; i < event_bytes_.size(); i++) {
            cout << hex << setfill('0') << setw(2) << (int)event_bytes_[i] << " ";
            if ((i + 1) % 10 == 0) cout << "\n   ";
        }
        cout << dec << "\n";
        cout << "======================================================\n";
        current_crc_errors++;
    }

public:
    EtrocChannelDebug(const string& name, uint32_t chip_id = 0x1ABCD) 
        : name_(name), expected_chip_id_(chip_id) {}
        
    void process_word(uint64_t w, uint8_t hw_type) {
        if (current_crc_errors >= max_crc_errors_to_print) return;
        
        FrameType payload_t = get_payload_type(w);

        if (payload_t == TRAILER && etroc::trl_chipid(w) != expected_chip_id_) {
            payload_t = UNKNOWN;
        }

        if (payload_t == DATA) {
            if (!in_event_) return; 
            current_event_data_.push_back(w); 
            push_word_bytes(w, 5); 
            return;
        }
        
        if (payload_t == HEADER) {
            in_event_ = true; 
            header_word_ = w;
            current_event_data_.clear(); 
            event_bytes_.clear();
            
            push_word_bytes(w, 5); 
            hdr_bcid_ = etroc::hdr_bcid(w); 
            return;
        }
        
        if (payload_t == TRAILER) {
            if (!in_event_) return; 
            
            uint16_t trl_h = etroc::trl_hits(w); 
            uint64_t event_hits = current_event_data_.size();

            uint64_t bcid_mismatch_count = 0;
            for (uint64_t hit_w : current_event_data_) {
                if (etroc::dat_bcid(hit_w) != hdr_bcid_) bcid_mismatch_count++;
            }
            
            push_word_bytes(w >> 8, 4);
            uint8_t calculated_crc = etroc::calculate_crc8(event_bytes_);
            uint8_t expected_crc = (w & 0xFF);
            bool is_crc_mismatch = (calculated_crc != expected_crc);
            
            bool is_merged_or_corrupted = (bcid_mismatch_count > 0);

            if (!is_merged_or_corrupted) {
                if (is_crc_mismatch) {
                    print_event_dump(w, calculated_crc, expected_crc);
                }
            }

            in_event_ = false;
            return;
        }
    }
};

int main(int argc, char* argv[]) {
    if (argc > 1) {
        max_crc_errors_to_print = atoi(argv[1]);
    }
    
    string folder = "daq_bin_results";
    vector<string> files;
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin") files.push_back(entry.path().filename().string());
        }
    }
    sort(files.begin(), files.end());

    if (files.empty()) {
        cout << "No .bin files found in " << folder << "\n";
        return 0;
    }

    cout << "Dumping up to " << max_crc_errors_to_print << " CRC error events...\n";

    for (const string& fn : files) {
        if (current_crc_errors >= max_crc_errors_to_print) break;
        
        ifstream f(folder + "/" + fn, ios::binary);
        if (!f.is_open()) continue;
        
        EtrocChannelDebug right_ch("RIGHT", 0x1ABCD), left_ch("LEFT", 0x1ABCD);
        
        uint32_t magic, size_words;
        uint64_t global_valid_cnt = 0;
        vector<HWDrop> pending_drops;

        vector<uint32_t> valid_quad;
        auto process_quad_word = [&](uint32_t w) {
            uint32_t marker = (w >> 29) & 0x3;
            if (marker != valid_quad.size()) {
                valid_quad.clear();
                if (marker == 0 && w != 0) valid_quad.push_back(w); 
            } else {
                valid_quad.push_back(w);
                if (valid_quad.size() == 4) {
                    uint64_t dr, dl; uint8_t t_r, t_l;
                    unpack_quad(valid_quad[0], valid_quad[1], valid_quad[2], valid_quad[3], dr, t_r, dl, t_l);
                    right_ch.process_word(dr, t_r); left_ch.process_word(dl, t_l);
                    valid_quad.clear();
                }
            }
        };

        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (current_crc_errors >= max_crc_errors_to_print) break;
            
            if (magic == MAGIC_LOG) {
                vector<uint32_t> logs(size_words); f.read((char*)logs.data(), size_words * 4);
                if (logs[0] == 0xDEADBEEF) continue;
                
                uint32_t limit = min((uint32_t)logs.size(), (logs[0] & 0xFFF) + 1); 
                vector<uint32_t> temp_payloads; 
                
                for (uint32_t i = 1; i < limit; i++) {
                    uint32_t lw = logs[i];
                    if ((lw >> 31) == 1) temp_payloads.push_back(lw);
                    else if ((lw >> 30) == 0) {
                        uint32_t drops = (lw >> 17) & 0x1FFF; 
                        uint32_t addr = lw & 0x1FFFF;         
                        pending_drops.push_back({addr, drops, -1, temp_payloads});
                        temp_payloads.clear();
                    } else {
                        int lap = (lw & 0x7) == 0 ? 7 : (lw & 0x7) - 1;
                        for (auto& d : pending_drops) { if (d.lap == -1) d.lap = lap; }
                    }
                }
                if (((logs[0] >> 29) & 1) && !temp_payloads.empty()) temp_payloads.clear();
                
            } else if (magic == MAGIC_DAT) {
                vector<uint32_t> d(size_words); f.read((char*)d.data(), size_words * 4);
                
                for (uint32_t w : d) {
                    uint32_t phys_addr = global_valid_cnt % 131072;
                    int cur_lap = (global_valid_cnt / 131072) % 8; 
                    global_valid_cnt++;

                    if (w == 0) { valid_quad.clear(); continue; }

                    for (auto it = pending_drops.begin(); it != pending_drops.end(); ) {
                        if (it->addr == phys_addr && (it->lap == cur_lap || it->lap == -1)) {
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
                double elapsed_sec;
                f.read((char*)&elapsed_sec, 8); 
            } else break;
        }
    }
    
    if (current_crc_errors == 0) {
         cout << "No CRC errors found in the analyzed data.\n";
    }
    
    return 0;
}
