#include <iostream>
#include <fstream>
#include <vector>
#include <iomanip>
#include <cstdint>
#include <string>
#include <filesystem>
#include <algorithm>
#include <cstring>
#include <cmath> // 🌟 Include sqrt function for standard deviation calculation

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
    inline uint16_t dat_counter_a(uint64_t d) { return d & 0x1FF; }
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

enum FrameType { HEADER = 0, DATA = 1, TRAILER = 2, FILLER = 3, UNKNOWN = 4 };

FrameType get_payload_type(uint64_t w) {
    if (etroc::is_data(w)) return DATA;
    if (etroc::is_header(w)) return HEADER;
    if (etroc::is_filler(w)) return FILLER;
    if (etroc::is_trailer(w)) return TRAILER;
    return UNKNOWN;
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
    double effective_bandwidth_mbps = 0.0;
    uint64_t hw_reported_drops = 0;
    uint64_t total_salvaged_words = 0;
    uint64_t words_injected = 0;
    uint64_t fail_address_drift_orphan = 0;
    uint64_t fail_word_order_quad_slip = 0;
};

struct RecoStats {
    uint64_t total_frames = 0, headers = 0, trailers = 0, fillers = 0, hits_raw = 0, valid_hits = 0;
    uint64_t events_missing_header = 0, orphan_data_words = 0;
    uint64_t words_dropped_at_start = 0;
    uint64_t wp_boundary_cuts = 0;
    
    uint64_t events_perfect_hits = 0, events_hit_mismatch = 0;
    uint64_t events_corrupted_bcid = 0, events_discarded_by_jump = 0;
    
    // L1 jump statistics variables
    uint64_t l1_counter_jumps = 0;
    uint64_t l1_jump_sum = 0;
    uint64_t l1_jump_sq_sum = 0; 
    
    // Counter A jump statistics variables
    uint64_t counter_a_jumps = 0;
    uint64_t counter_a_jump_sum = 0;
    uint64_t counter_a_jump_sq_sum = 0; 
    
    uint64_t match_intra_colrow = 0, drop_intra_colrow = 0;
    uint64_t match_hdr_bcid = 0, drop_hdr_bcid = 0, drop_hdr_bcid_only = 0;
    
    uint64_t ea_counts[4] = {0}, fatal_ea_errors = 0, crc_match = 0, crc_mismatch = 0;
    uint64_t expected_total_hits = 0, sw_lost_hits = 0;
    uint64_t header_type_counts[4] = {0}, buffer_status_counts[4] = {0};
    uint64_t hit_map[16][16] = {0};
    uint64_t fake_trailers_blocked = 0;
    uint64_t unknown_frames = 0;
    
    // Recovered error detail tracking variables
    uint64_t crc_err_ea = 0;
    uint64_t crc_err_missing = 0;
    uint64_t crc_err_missing_with_ea = 0;
    uint64_t crc_err_silent = 0;
    uint64_t events_with_missing_hits = 0;
    uint64_t events_missing_hits_and_crc_err = 0;
    uint64_t events_missing_hits_but_crc_ok = 0;
    uint64_t events_with_ea_errors = 0;
    uint64_t events_with_ea_and_crc_err = 0;
};

class EtrocChannelAnalyzer {
    string name_;
    uint32_t expected_chip_id_;
    RecoStats s_;
    uint64_t last_processed_word_ = 0xFFFFFFFFFFFFFFFFULL; // 🌟 Previous word memory variable for debouncing
    bool is_synced_ = false;
    bool expect_l1_jump_ = false;
    bool in_event_ = false;
    bool has_pending_event_ = false;
    
    uint64_t pending_trailer_w_ = 0;
    uint32_t hdr_bcid_ = 0;
    uint8_t  hdr_type_ = 0;
    uint8_t  last_l1_cnt_ = 0;
    bool     is_first_event_ = true;
    
    std::vector<uint64_t> current_event_data_;
    std::vector<uint8_t> event_bytes_;
    
    bool     first_counter_a_[16][16];
    uint16_t last_counter_a_[16][16];

    void push_word_bytes(std::vector<uint8_t>& target_vec, uint64_t w) {
        for (int i = 4; i >= 0; i--) {
            target_vec.push_back((w >> (i * 8)) & 0xFF);
        }
    }

    void commit_pending_event() {
        s_.header_type_counts[hdr_type_]++;
        uint16_t trl_h = etroc::trl_hits(pending_trailer_w_);
        uint8_t status = etroc::trl_status(pending_trailer_w_);
        s_.buffer_status_counts[(status >> 4) & 0x3]++;
        
        uint64_t event_hits = current_event_data_.size();
        if (trl_h == 0 && event_hits >= 200) trl_h = 256;
        
        bool current_event_has_ea_error = false;
        uint64_t bcid_mismatch_count = 0;
        
        for (uint64_t hit_w : current_event_data_) {
            uint8_t ea = etroc::dat_ea(hit_w);
            s_.ea_counts[ea & 0x3]++;
            if (ea >= 0x2) {
                s_.fatal_ea_errors++;
                current_event_has_ea_error = true;
            }
            
            uint8_t col = etroc::dat_col1(hit_w);
            uint8_t row = etroc::dat_row1(hit_w);
            bool intra_ok = (col == etroc::dat_col2(hit_w) && row == etroc::dat_row2(hit_w));
            bool bcid_ok = (etroc::dat_bcid(hit_w) == hdr_bcid_);
            
            if (intra_ok) s_.match_intra_colrow++;
            else s_.drop_intra_colrow++;
            
            if (bcid_ok) s_.match_hdr_bcid++;
            else {
                s_.drop_hdr_bcid++;
                bcid_mismatch_count++;
                if (intra_ok) s_.drop_hdr_bcid_only++;
            }
            
            if (intra_ok) {
                uint16_t cnt_a = etroc::dat_counter_a(hit_w);
                if (first_counter_a_[row][col]) {
                    first_counter_a_[row][col] = false;
                } else {
                    uint16_t expected_cnt = (last_counter_a_[row][col] + 1) & 0x1FF;
                    if (cnt_a != expected_cnt) {
                        uint64_t j_size = (cnt_a - expected_cnt) & 0x1FF;
                        s_.counter_a_jumps++;
                        s_.counter_a_jump_sum += j_size;
                        s_.counter_a_jump_sq_sum += (j_size * j_size);
                    }
                }
                last_counter_a_[row][col] = cnt_a;
                
                if (bcid_ok && ea < 0x2) {
                    s_.valid_hits++;
                    s_.hit_map[row][col]++;
                }
            }
        }
        
        if (current_event_has_ea_error) {
            s_.events_with_ea_errors++;
        }
        
        push_word_bytes(event_bytes_, pending_trailer_w_);
        bool is_crc_mismatch = (etroc::calculate_crc8(event_bytes_) != 0x00);
        bool is_merged_or_corrupted = (bcid_mismatch_count > 0);
        
        if (is_merged_or_corrupted) {
            s_.events_corrupted_bcid++;
        } else {
            s_.expected_total_hits += trl_h;
            bool has_missing_hits = (trl_h > event_hits);
            
            if (has_missing_hits) {
                s_.events_hit_mismatch++;
                s_.sw_lost_hits += (trl_h - event_hits);
            } else {
                s_.events_perfect_hits++;
            }
            
            if (!is_crc_mismatch) {
                s_.crc_match++;
            } else {
                s_.crc_mismatch++;
                bool is_silent = true;
                if (current_event_has_ea_error) { s_.crc_err_ea++; is_silent = false; }
                if (has_missing_hits) { s_.crc_err_missing++; is_silent = false; }
                if (has_missing_hits && current_event_has_ea_error) s_.crc_err_missing_with_ea++;
                if (is_silent) s_.crc_err_silent++;
            }
            
            if (has_missing_hits) {
                s_.events_with_missing_hits++;
                if (is_crc_mismatch) s_.events_missing_hits_and_crc_err++;
                else s_.events_missing_hits_but_crc_ok++;
            }
            
            if (current_event_has_ea_error) {
                s_.events_with_ea_errors++;
                if (is_crc_mismatch) s_.events_with_ea_and_crc_err++;
            }
        }
    }

public:
    EtrocChannelAnalyzer(const string& name, uint32_t chip_id = 0x1FFFE) 
        : name_(name), expected_chip_id_(chip_id) 
    {
        memset(first_counter_a_, true, sizeof(first_counter_a_));
        memset(last_counter_a_, 0, sizeof(last_counter_a_));
    }

    void force_resync() {
        if (in_event_ || has_pending_event_) s_.wp_boundary_cuts++;
        in_event_ = false;
        has_pending_event_ = false;
        is_synced_ = false;
        expect_l1_jump_ = false;
    }

    void finalize_stream() {
        if (has_pending_event_) {
            commit_pending_event();
            has_pending_event_ = false;
        }
        if (in_event_) {
            s_.wp_boundary_cuts++;
            in_event_ = false;
        }
    }

    void process_word(uint64_t w) {
        s_.total_frames++;
        // 🌟 [Core Software Shield] Completely block Hardware Stutter (duplication bug)!
        if (w == last_processed_word_) {
            // If it is 100% identical to the just processed word, 
            // it is not sent by ETROC2, but a duplicated garbage due to FPGA clock mismatch.
            return; // Ignore it cleanly!
        }
        last_processed_word_ = w; // Remember current word


        FrameType payload_t = get_payload_type(w);
        
        if (payload_t == TRAILER && etroc::trl_chipid(w) != expected_chip_id_) {
            s_.fake_trailers_blocked++;
            payload_t = UNKNOWN;
        }
        
        if (payload_t == UNKNOWN) {
            s_.unknown_frames++;
        }
        
        if (!is_synced_) {
            if (payload_t == HEADER) is_synced_ = true;
            else { s_.words_dropped_at_start++; return; }
        }
        
        if (payload_t == DATA) {
            s_.hits_raw++;
            if (!in_event_) { s_.orphan_data_words++; return; }
            current_event_data_.push_back(w);
            push_word_bytes(event_bytes_, w);
            return;
        }
        
        if (payload_t == HEADER) {
            s_.headers++;
            uint8_t curr_l1_cnt = etroc::hdr_l1_cnt(w);
            bool l1_jump_detected = false;
            
            if (in_event_) {
                s_.wp_boundary_cuts++;
                expect_l1_jump_ = true;
            }
            
            if (!is_first_event_ && !expect_l1_jump_) {
                uint8_t expected_l1_cnt = (last_l1_cnt_ + 1) & 0xFF;
                if (curr_l1_cnt != expected_l1_cnt) {
                    uint64_t j_size = (curr_l1_cnt - last_l1_cnt_) & 0xFF;
                    s_.l1_counter_jumps++;
                    s_.l1_jump_sum += j_size;
                    s_.l1_jump_sq_sum += (j_size * j_size); 
                    l1_jump_detected = true;
                }
            }
            is_first_event_ = false;
            last_l1_cnt_ = curr_l1_cnt;
            expect_l1_jump_ = false;
            
            if (has_pending_event_) {
                if (l1_jump_detected) s_.events_discarded_by_jump++;
                else commit_pending_event();
                has_pending_event_ = false;
            }
            
            in_event_ = true;
            current_event_data_.clear();
            event_bytes_.clear();
            push_word_bytes(event_bytes_, w);
            
            hdr_bcid_ = etroc::hdr_bcid(w);
            hdr_type_ = etroc::hdr_type(w);
            return;
        }
        
        if (payload_t == TRAILER) {
            s_.trailers++;
            if (!in_event_) {
                s_.events_missing_header++;
                return;
            }
            pending_trailer_w_ = w;
            has_pending_event_ = true;
            in_event_ = false;
            return;
        }
        
        if (payload_t == FILLER) {
            s_.fillers++;
            return;
        }
    }

    void export_python_data(const string& base_filename, const SystemStats& sys) const {
        string prefix = base_filename.substr(0, base_filename.find(".bin"));
        string hm_file = prefix + "_" + name_ + "_heatmap.csv";
        ofstream hmf(hm_file);
        for (int r = 0; r < 16; r++) {
            for (int c = 0; c < 16; c++) hmf << s_.hit_map[r][c] << (c == 15 ? "" : ",");
            hmf << "\n";
        }

        double bufempty_ratio = sys.total_32b_words > 0 ? (double)sys.total_ram_buffer_empty / sys.total_32b_words * 100.0 : 0.0;
        
        double l1_jump_avg = 0.0, l1_jump_std = 0.0;
        if (s_.l1_counter_jumps > 0) {
            l1_jump_avg = (double)s_.l1_jump_sum / s_.l1_counter_jumps;
            double l1_jump_sq_avg = (double)s_.l1_jump_sq_sum / s_.l1_counter_jumps;
            double var = l1_jump_sq_avg - (l1_jump_avg * l1_jump_avg);
            l1_jump_std = var > 0 ? sqrt(var) : 0.0;
        }
        
        double cnt_a_jump_avg = 0.0, cnt_a_jump_std = 0.0;
        if (s_.counter_a_jumps > 0) {
            cnt_a_jump_avg = (double)s_.counter_a_jump_sum / s_.counter_a_jumps;
            double cnt_a_jump_sq_avg = (double)s_.counter_a_jump_sq_sum / s_.counter_a_jumps;
            double var = cnt_a_jump_sq_avg - (cnt_a_jump_avg * cnt_a_jump_avg);
            cnt_a_jump_std = var > 0 ? sqrt(var) : 0.0;
        }

        uint64_t total_complete = s_.events_perfect_hits + s_.events_hit_mismatch + s_.events_corrupted_bcid + s_.events_discarded_by_jump;
        
        string st_file = prefix + "_" + name_ + "_stats.csv";
        ofstream stf(st_file);
        stf << "Category,Metric,Value\n"
            << "FRAME,Total_Frames," << s_.total_frames << "\n"
            << "FRAME,Raw_Hit_Frames," << s_.hits_raw << "\n"
            << "FRAME,Headers," << s_.headers << "\n"
            << "FRAME,Trailers," << s_.trailers << "\n"
            << "FRAME,Fillers," << s_.fillers << "\n"
            << "FRAME,Fake_Trailers_Blocked," << s_.fake_trailers_blocked << "\n"
            << "FRAME,Unknown_Frames," << s_.unknown_frames << "\n"
            << "EXCLUDE,WP_Boundary_Cuts," << s_.wp_boundary_cuts << "\n"
            << "EXCLUDE,Words_Dropped_At_Start," << s_.words_dropped_at_start << "\n"
            << "EXCLUDE,Events_Missing_Header," << s_.events_missing_header << "\n"
            << "EXCLUDE,Orphan_Data_Words," << s_.orphan_data_words << "\n"
            << "EVENT,Total_Complete_Events," << total_complete << "\n"
            << "EVENT,Events_Perfect_Hits," << s_.events_perfect_hits << "\n"
            << "EVENT,Events_Corrupted_BCID," << s_.events_corrupted_bcid << "\n"
            << "EVENT,Events_Hit_Mismatch," << s_.events_hit_mismatch << "\n"
            << "EVENT,Events_Discarded_By_Jump," << s_.events_discarded_by_jump << "\n"
            << "EVENT,L1_Counter_Jumps," << s_.l1_counter_jumps << "\n"
            << "EVENT,L1_Jump_Avg," << l1_jump_avg << "\n"
            << "EVENT,L1_Jump_Std," << l1_jump_std << "\n"
            << "EVENT,Header_Type_Regular," << s_.header_type_counts[0] << "\n"
            << "EVENT,Header_Type_Random," << s_.header_type_counts[1] << "\n"
            << "EVENT,Header_Type_Counter," << s_.header_type_counts[2] << "\n"
            << "EVENT,Header_Type_Reserved," << s_.header_type_counts[3] << "\n"
            << "EVENT,Buffer_Normal," << s_.buffer_status_counts[0] << "\n"
            << "EVENT,Buffer_HalfFull," << s_.buffer_status_counts[1] << "\n"
            << "EVENT,Buffer_Overflow," << s_.buffer_status_counts[2] << "\n"
            << "EVENT,Buffer_Full," << s_.buffer_status_counts[3] << "\n"
            << "EVENT,CRC_Match," << s_.crc_match << "\n"
            << "EVENT,CRC_Mismatch," << s_.crc_mismatch << "\n"
            << "EVENT,CRC_Err_MissingData," << s_.crc_err_missing << "\n"
            << "EVENT,CRC_Err_EA_Fatal," << s_.crc_err_ea << "\n"
            << "EVENT,CRC_Err_MissingData_With_EA," << s_.crc_err_missing_with_ea << "\n"
            << "EVENT,CRC_Err_SilentFlip," << s_.crc_err_silent << "\n"
            << "EVENT,Events_With_Missing_Hits," << s_.events_with_missing_hits << "\n"
            << "EVENT,Events_Missing_Hits_And_CRC_Err," << s_.events_missing_hits_and_crc_err << "\n"
            << "EVENT,Events_Missing_Hits_But_CRC_OK," << s_.events_missing_hits_but_crc_ok << "\n"
            << "EVENT,Events_With_EA_Errors," << s_.events_with_ea_errors << "\n"
            << "EVENT,Events_With_EA_and_CRC_Err," << s_.events_with_ea_and_crc_err << "\n"
            << "HIT,Expected_Total_Hits," << s_.expected_total_hits << "\n"
            << "HIT,Valid_Hits," << s_.valid_hits << "\n"
            << "HIT,Lost_Hits," << s_.sw_lost_hits << "\n"
            << "HIT,BCID_Match," << s_.match_hdr_bcid << "\n"
            << "HIT,BCID_Drop," << s_.drop_hdr_bcid << "\n"
            << "HIT,Intra_Match," << s_.match_intra_colrow << "\n"
            << "HIT,Intra_Drop," << s_.drop_intra_colrow << "\n"
            << "HIT,BCID_Mismatch_Only," << s_.drop_hdr_bcid_only << "\n"
            << "HIT,Counter_A_Jumps," << s_.counter_a_jumps << "\n"
            << "HIT,Counter_A_Lost_Avg," << cnt_a_jump_avg << "\n"
            << "HIT,Counter_A_Lost_Std," << cnt_a_jump_std << "\n"
            << "HIT,EA_00_Clean," << s_.ea_counts[0] << "\n"
            << "HIT,EA_01_Corrected," << s_.ea_counts[1] << "\n"
            << "HIT,EA_10_Uncorrectable," << s_.ea_counts[2] << "\n"
            << "HIT,EA_11_Undefined," << s_.ea_counts[3] << "\n"
            << "SYSTEM,Link_Bandwidth_Mbps," << sys.bandwidth_mbps << "\n"
            << "SYSTEM,Effective_Bandwidth_Mbps," << sys.effective_bandwidth_mbps << "\n"
            << "SYSTEM,Total_HW_Drops," << sys.hw_reported_drops << "\n"
            << "SYSTEM,Salvaged_Words," << sys.total_salvaged_words << "\n"
            << "SYSTEM,Injected_Words," << sys.words_injected << "\n"
            << "SYSTEM,Fail_Address_Drift," << sys.fail_address_drift_orphan << "\n"
            << "SYSTEM,Fail_Word_Order_Slips," << sys.fail_word_order_quad_slip << "\n"
            << "SYSTEM,RAM_Buffer_Empty_Words," << sys.total_ram_buffer_empty << "\n"
            << "SYSTEM,RAM_Buffer_Empty_Ratio," << bufempty_ratio << "\n";
    }

    void print_terminal_summary() const {
        double l1_jump_avg = 0.0, l1_jump_std = 0.0;
        if (s_.l1_counter_jumps > 0) {
            l1_jump_avg = (double)s_.l1_jump_sum / s_.l1_counter_jumps;
            double var = ((double)s_.l1_jump_sq_sum / s_.l1_counter_jumps) - (l1_jump_avg * l1_jump_avg);
            l1_jump_std = var > 0 ? sqrt(var) : 0.0;
        }
        
        double cnt_a_jump_avg = 0.0, cnt_a_jump_std = 0.0;
        if (s_.counter_a_jumps > 0) {
            cnt_a_jump_avg = (double)s_.counter_a_jump_sum / s_.counter_a_jumps;
            double var = ((double)s_.counter_a_jump_sq_sum / s_.counter_a_jumps) - (cnt_a_jump_avg * cnt_a_jump_avg);
            cnt_a_jump_std = var > 0 ? sqrt(var) : 0.0;
        }

        uint64_t total_complete = s_.events_perfect_hits + s_.events_hit_mismatch + s_.events_corrupted_bcid + s_.events_discarded_by_jump;
        
        cout << "  [" << name_ << " CHANNEL - COMPLETION REPORT]\n";
        cout << "   ├─ Complete Events : " << total_complete << " (Perfect: " << s_.events_perfect_hits << " | Mismatch: " << s_.events_hit_mismatch << ")\n";
        cout << "   ├─ 🚨 Discarded (L1)  : " << s_.events_discarded_by_jump << " (Avg Jump: " << fixed << setprecision(3) << l1_jump_avg << " ± " << l1_jump_std << ")\n";
        cout << "   ├─ 📈 Pixel Cnt_A Jumps: " << s_.counter_a_jumps << " (Avg Lost: " << fixed << setprecision(3) << cnt_a_jump_avg << " ± " << cnt_a_jump_std << ")\n";
        cout << "   ├─ 🪓 WP Boundary Cuts : " << s_.wp_boundary_cuts << " times\n";
        cout << "   ├─ 🧹 Fake Trl Blocked : " << s_.fake_trailers_blocked << " times\n";
        cout << "   └─ L1 Buff [Norm:" << s_.buffer_status_counts[0] << " | Half:" << s_.buffer_status_counts[1] << " | Ovf:" << s_.buffer_status_counts[2] << " | Full:" << s_.buffer_status_counts[3] << "]\n";
    }
};

int extract_batch_number(const std::string& filename) {
    size_t pos = filename.find("batch_");
    if (pos != std::string::npos) {
        size_t start = pos + 6;
        size_t end = filename.find_first_not_of("0123456789", start);
        if (start != std::string::npos && end != start)
            return std::stoi(filename.substr(start, end - start));
    }
    size_t start = filename.find_first_of("0123456789");
    if (start != std::string::npos) return std::stoi(filename.substr(start));
    return 0;
}

bool compare_batch_files(const std::string& a, const std::string& b) {
    return extract_batch_number(a) < extract_batch_number(b);
}

int main() {
    string folder = "daq_bin_results";
    vector<string> files;
    
    if (fs::exists(folder)) {
        for (const auto& entry : fs::directory_iterator(folder)) {
            if (entry.path().extension() == ".bin")
                files.push_back(entry.path().filename().string());
        }
    }
    
    sort(files.begin(), files.end(), compare_batch_files);
    
    if (files.empty()) {
        cout << "❌ No .bin files found in daq_bin_results/\n";
        return 1;
    }

    for (const string& fn : files) {
        ifstream f(folder + "/" + fn, ios::binary);
        if (!f.is_open()) continue;
        
        cout << "\n>>> 🚀 Analyzing: " << fn << " ...\n";
        
        EtrocChannelAnalyzer right_ch("RIGHT", 0x1FFFE);
        EtrocChannelAnalyzer left_ch("LEFT", 0x1FFFE);
        SystemStats sys;
        
        uint32_t magic, size_words;
        uint64_t global_valid_cnt = 0;
        vector<HWDrop> pending_drops;
        bool first_log_skipped = false;

        // 🌟 Temporary assembly buffer for channel splitting (state must be maintained across loops)
        uint64_t right_low = 0, left_low = 0;
        bool right_low_ready = false, left_low_ready = false;

        // 🌟 Smart routing word processor (completely removed 4-word Quad dependency)
        auto process_dpram_word = [&](uint32_t w) {
            if (w == 0) { 
                sys.total_ram_buffer_empty++; 
                return; 
            }
            
            // 1. Status registers (Drop / Lap) are not actual Payload data, so skip them
            if ((w >> 31) == 0) return; 

            // 2. Data word (MSB == 1), branch channel by top 3 bits
            uint8_t channel_and_part = (w >> 29) & 0x7; 
            
            switch (channel_and_part) {
                // ==========================
                // 🔴 Right channel routing
                // ==========================
                case 0b100: // Right Low (24 bits)
                    right_low = w & 0xFFFFFF;
                    right_low_ready = true;
                    break;
                    
                case 0b101: // Right High (16 bits)
                    if (right_low_ready) {
                        uint64_t right_high = w & 0xFFFF;
                        uint64_t full_word_right = (right_high << 24) | right_low;
                        right_ch.process_word(full_word_right);
                        right_low_ready = false;
                    } else {
                        // If High arrives without a pair, process error flag and reset synchronization for this channel only
                        sys.fail_word_order_quad_slip++;
                        right_ch.force_resync();
                    }
                    break;
                    
                // ==========================
                // 🔵 Left channel routing
                // ==========================
                case 0b110: // Left Low (24 bits)
                    left_low = w & 0xFFFFFF;
                    left_low_ready = true;
                    break;
                    
                case 0b111: // Left High (16 bits)
                    if (left_low_ready) {
                        uint64_t left_high = w & 0xFFFF;
                        uint64_t full_word_left = (left_high << 24) | left_low;
                        left_ch.process_word(full_word_left);
                        left_low_ready = false;
                    } else {
                        // If High arrives without a pair, process error flag and reset synchronization for this channel only
                        sys.fail_word_order_quad_slip++;
                        left_ch.force_resync();
                    }
                    break;
                    
                default:
                    // Identifier error
                    sys.fail_word_order_quad_slip++;
                    break;
            }
        };

        while (f.read((char*)&magic, 4) && f.read((char*)&size_words, 4)) {
            if (magic == MAGIC_LOG) {
                vector<uint32_t> logs(size_words);
                f.read((char*)logs.data(), size_words * 4);
                
                if (logs[0] == 0xDEADBEEF) continue;
                if (!first_log_skipped) { first_log_skipped = true; continue; }
                
                const uint32_t ADDR_WIDTH = 12;
                const uint32_t ADDR_MASK = (1 << ADDR_WIDTH) - 1;
                uint32_t valid_count = logs[0] & ADDR_MASK;
                uint32_t limit = min((uint32_t)logs.size(), valid_count + 1);
                
                vector<uint32_t> temp_payloads;
                
                for (uint32_t i = 1; i < limit; i++) {
                    uint32_t lw = logs[i];
                    if ((lw >> 31) == 1) {
                        temp_payloads.push_back(lw);
                    } else if ((lw >> 30) == 0) {
                        uint32_t drops = (lw >> 18) & 0xFFF;
                        uint32_t addr  = lw & 0x3FFFF;
                        sys.hw_reported_drops += drops;
                        sys.total_salvaged_words += temp_payloads.size();
                        pending_drops.push_back({addr, drops, -1, temp_payloads});
                        temp_payloads.clear();
                    } else {
                        int lap = (lw & 0x7) == 0 ? 7 : (lw & 0x7) - 1;
                        for (auto& d : pending_drops) {
                            if (d.lap == -1) d.lap = lap;
                        }
                    }
                }
                bool log_overflowed = ((logs[0] >> 29) & 1) == 1;
                if (log_overflowed && !temp_payloads.empty()) temp_payloads.clear();
                
            } else if (magic == MAGIC_DAT) {
                vector<uint32_t> d(size_words);
                f.read((char*)d.data(), size_words * 4);
                sys.total_32b_words += size_words;
                
                for (uint32_t w : d) {
                    uint32_t phys_addr = global_valid_cnt % 262144;
                    int cur_lap = (global_valid_cnt / 262144) % 8;
                    global_valid_cnt++;
                    
                    for (auto it = pending_drops.begin(); it != pending_drops.end(); ) {
                        if (it->addr == phys_addr && (it->lap == cur_lap || it->lap == -1)) {
                            sys.words_injected += it->salvaged_payloads.size();
                            for (uint32_t sw : it->salvaged_payloads) 
                                process_dpram_word(sw); 
                            it = pending_drops.erase(it);
                            break;
                        } else {
                            ++it;
                        }
                    }
                    process_dpram_word(w);
                }
                
            } else if (magic == MAGIC_META) {
                f.read((char*)&sys.elapsed_sec, 8);
                sys.bandwidth_mbps = (sys.total_32b_words * 32.0) / 1000000.0 / sys.elapsed_sec;
                uint64_t valid_words = sys.total_32b_words > sys.total_ram_buffer_empty ? (sys.total_32b_words - sys.total_ram_buffer_empty) : 0;
                sys.effective_bandwidth_mbps = (valid_words * 32.0) / 1000000.0 / sys.elapsed_sec;
            } else break;
        }
        
        right_ch.finalize_stream();
        left_ch.finalize_stream();
        
        for (const auto& pd : pending_drops) sys.fail_address_drift_orphan += pd.salvaged_payloads.size();
        
        cout << "  =======================================================\n";
        cout << "  📡 [ SYSTEM PERFORMANCE & RECOVERY ]\n";
        cout << "   ├─ Link Bandwidth   : " << fixed << setprecision(2) << sys.bandwidth_mbps << " Mbps (Including Empty Buffers)\n";
        cout << "   ├─ 🚀 Effective BW  : " << fixed << setprecision(2) << sys.effective_bandwidth_mbps << " Mbps (Pure Payload)\n";
        cout << "   ├─ HW Drops Reported: " << sys.hw_reported_drops << " words\n";
        cout << "   ├─ ✅ Successful Inj: " << sys.words_injected << " words\n";
        cout << "   ├─ ❌ Failed Recovery: Drift=" << sys.fail_address_drift_orphan << ", Slip=" << sys.fail_word_order_quad_slip << "\n";
        cout << "  =======================================================\n";
        
        right_ch.print_terminal_summary();
        left_ch.print_terminal_summary();
        
        right_ch.export_python_data(fn, sys);
        left_ch.export_python_data(fn, sys);
    }
    return 0;
}