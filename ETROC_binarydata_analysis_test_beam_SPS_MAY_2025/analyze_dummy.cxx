#include <iostream>
#include <fstream>
#include <vector>
#include <cstdint>
#include <string>
#include <iomanip>

using namespace std;

const uint32_t MAGIC_LOG  = 0xAAAA0000;
const uint32_t MAGIC_DAT  = 0xBBBB0000;
const uint32_t MAGIC_META = 0xCCCC0000;

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };

FrameType get_payload_type(uint64_t w) {
    bool is_data = (w >> 39) & 1;
    if (is_data) return DATA;
    
    uint64_t sync = (w >> 24) & 0x7FFF;
    if (sync == 0x3C5C) {
        uint8_t type = (w >> 22) & 0x3;
        if (type == 0x0) return HEADER;
        if (type == 0x2) return FILLER;
    } else {
        return TRAILER;
    }
    return UNKNOWN;
}

// VHDL-matched CRC-8 calculation (Polynomial: x^8 + x^2 + x + 1 => 0x07)
uint8_t calc_crc8_vhdl(uint64_t data, uint8_t prev_crc) {
    uint8_t crc = prev_crc;
    for (int i = 39; i >= 0; i--) {
        uint8_t bit = (data >> i) & 1;
        uint8_t inv = ((crc >> 7) & 1) ^ bit;
        crc = (crc << 1);
        if (inv) {
            crc ^= 0x07;
        }
    }
    return crc;
}

inline void unpack_quad(uint32_t w1, uint32_t w2, uint32_t w3, uint32_t w4, 
                        uint64_t& d_right, uint64_t& d_left) {
    d_right = ((uint64_t)(w2 & 0xFFFFu) << 24) | (uint64_t)(w1 & 0xFFFFFFu);
    d_left  = ((uint64_t)(w4 & 0xFFFFu) << 24) | (uint64_t)(w3 & 0xFFFFFFu);
}

struct Stats {
    uint64_t headers = 0;
    uint64_t data_words = 0;
    uint64_t trailers = 0;
    uint64_t fillers = 0;
    uint64_t unknowns = 0;
    
    uint64_t l1_jumps = 0;
    uint64_t crc_matches = 0;
    uint64_t crc_mismatches = 0;
};

class EtrocChannel {
    string name_;
    Stats stats;
    
    bool in_event = false;
    uint8_t current_crc = 0x00;
    uint8_t last_l1_cnt = 0;
    bool is_first_event = true;

public:
    EtrocChannel(string name) : name_(name) {}
    
    void process_word(uint64_t w) {
        FrameType t = get_payload_type(w);
        
        if (t == HEADER) {
            stats.headers++;
            current_crc = calc_crc8_vhdl(w, 0x00);
            in_event = true;
            
            uint8_t l1_cnt = (w >> 14) & 0xFF;
            if (!is_first_event) {
                uint8_t expected = (last_l1_cnt + 1) & 0xFF;
                if (l1_cnt != expected) {
                    stats.l1_jumps++;
                }
            }
            last_l1_cnt = l1_cnt;
            is_first_event = false;
        } 
        else if (t == DATA) {
            stats.data_words++;
            if (in_event) {
                current_crc = calc_crc8_vhdl(w, current_crc);
            }
        } 
        else if (t == TRAILER) {
            stats.trailers++;
            if (in_event) {
                uint8_t trailer_crc = w & 0xFF;
                if (trailer_crc == current_crc) {
                    stats.crc_matches++;
                } else {
                    stats.crc_mismatches++;
                }
                in_event = false;
            }
        } 
        else if (t == FILLER) {
            stats.fillers++;
        } 
        else {
            stats.unknowns++;
        }
    }
    
    void print_report() {
        cout << "  === [" << name_ << " Channel] ===\n";
        cout << "  [1] Frame Statistics:\n";
        cout << "      Headers  : " << stats.headers << "\n";
        cout << "      Data     : " << stats.data_words << "\n";
        cout << "      Trailers : " << stats.trailers << "\n";
        cout << "      Fillers  : " << stats.fillers << "\n";
        cout << "      Unknown  : " << stats.unknowns << "\n";
        cout << "  [2] L1 Counter Analysis:\n";
        cout << "      L1 Jumps : " << stats.l1_jumps << "\n";
        cout << "  [3] CRC Analysis:\n";
        cout << "      Matches  : " << stats.crc_matches << "\n";
        cout << "      Mismatches: " << stats.crc_mismatches << "\n\n";
    }
};

struct HWDrop { 
    uint32_t addr; uint32_t count; int lap; vector<uint32_t> salvaged_payloads; 
};

int main(int argc, char* argv[]) {
    if (argc < 2) {
        cout << "Usage: " << argv[0] << " <input_file1.bin> [input_file2.bin ...]\n";
        return 1;
    }

    for (int i = 1; i < argc; i++) {
        string fn = argv[i];
        ifstream f(fn, ios::binary);
        if (!f.is_open()) {
            cout << "Failed to open " << fn << "\n";
            continue;
        }
        
        cout << ">>> Analyzing: " << fn << "\n";
        
        EtrocChannel right_ch("RIGHT"), left_ch("LEFT");
        
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
                    uint64_t dr, dl;
                    unpack_quad(valid_quad[0], valid_quad[1], valid_quad[2], valid_quad[3], dr, dl);
                    right_ch.process_word(dr); 
                    left_ch.process_word(dl);
                    valid_quad.clear();
                }
            }
        };

        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (magic == MAGIC_LOG) {
                vector<uint32_t> logs(size_words); f.read((char*)logs.data(), size_words * 4);
                if (logs[0] == 0xDEADBEEF) continue;
                
                uint32_t limit = min((uint32_t)logs.size(), (logs[0] & 0xFFF) + 1); 
                vector<uint32_t> temp_payloads; 
                
                for (uint32_t j = 1; j < limit; j++) {
                    uint32_t lw = logs[j];
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
        
        right_ch.print_report();
        left_ch.print_report();
    }
    
    return 0;
}
